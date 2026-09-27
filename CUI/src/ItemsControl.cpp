#include "ItemsControl.h"
#include "DependencyPropertyInfrastructure.h"
#include "StyleInfrastructure.h"
#include "TreeInfrastructure.h"

#include "Window.h"
#include "Label.h"
#include "ContentPresenter.h"
#include "Layout/OverlayLayout.h"
#include "Layout/StackPanel.h"
#include "Layout/WrapPanel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if CUI_ENABLE_DYNAMIC_XAML
namespace cui::design
{
	const std::wstring& AuthoredBindingListItemTypeName(
		const IBindingList& source) noexcept;
}
#endif
#include <vector>

namespace
{
	template<typename TCallback>
	class ItemsScopeExit final
	{
	public:
		explicit ItemsScopeExit(TCallback callback)
			: _callback(std::move(callback)) {}
		ItemsScopeExit(const ItemsScopeExit&) = delete;
		ItemsScopeExit& operator=(const ItemsScopeExit&) = delete;
		~ItemsScopeExit() { _callback(); }

	private:
		TCallback _callback;
	};

	template<typename TCallback>
	ItemsScopeExit(TCallback) -> ItemsScopeExit<TCallback>;

	bool SameCompiledBindingPath(
		CompiledBindingPathView left,
		CompiledBindingPathView right) noexcept
	{
		return left.Version == right.Version
			&& left.Steps.data() == right.Steps.data()
			&& left.Steps.size() == right.Steps.size();
	}

	void ValidateCompiledMemberPath(
		CompiledBindingPathView path,
		const char* propertyName)
	{
		if (path.Version != CompiledBindingPathVersion)
			throw std::invalid_argument(std::string(propertyName)
				+ " compiled path version is unsupported");
		for (const auto& step : path.Steps)
		{
			if (!HasCompiledBindingPathCapability(step.Capabilities,
				CompiledBindingPathCapabilities::Read))
				throw std::invalid_argument(std::string(propertyName)
					+ " compiled path requires read capability");
			if (step.Kind == CompiledBindingPathStepKind::Property
				&& !step.Property)
				throw std::invalid_argument(std::string(propertyName)
					+ " compiled property steps require property tokens");
		}
	}

	bool VisualSubtreeContains(
		const Control* root,
		const Control* candidate) noexcept
	{
		if (!root || !candidate) return false;
		for (auto* current = candidate; current;
			current = current->GetVisualParent())
		{
			if (current == root) return true;
		}
		return false;
	}

	class MaterializedItemsSourceSnapshot final
		: public IBindingList,
		  public IBindingListOccurrenceIdentity,
		  public IBindingListStableSnapshot,
		  public IBindingListGroupView
	{
		struct Entry final
		{
			BindingSourceReference Item;
			size_t OccurrenceIdentity = 0;
		};
		struct Node final
		{
			std::shared_ptr<const Node> Left;
			std::shared_ptr<const std::vector<Entry>> Page;
			std::shared_ptr<const Node> Right;
			uint64_t Priority = 0;
			size_t Count = 0;
		};
		using Root = std::shared_ptr<const Node>;
		static constexpr size_t PageCapacity = 256;

	public:
		MaterializedItemsSourceSnapshot(
			DataTypeToken itemTypeToken,
#if CUI_ENABLE_DYNAMIC_XAML
			std::wstring itemTypeName,
#endif
			std::vector<BindingSourceReference> items,
			std::vector<size_t> occurrenceIdentities,
			bool hasOccurrenceIdentities,
			std::vector<BindingListGroup> groups)
			: _itemTypeToken(itemTypeToken),
#if CUI_ENABLE_DYNAMIC_XAML
			  _itemTypeName(std::move(itemTypeName)),
#endif
			  _hasOccurrenceIdentities(hasOccurrenceIdentities
				  && occurrenceIdentities.size() == items.size()),
			  _groups(std::move(groups))
		{
			std::vector<Entry> entries;
			entries.reserve(items.size());
			for (size_t index = 0; index < items.size(); ++index)
				entries.push_back({ std::move(items[index]),
					_hasOccurrenceIdentities
						? occurrenceIdentities[index] : 0 });
			_root = Build(std::move(entries));
		}

		size_t Count() const noexcept override { return NodeCount(_root); }
		bool TryGetItem(
			size_t index,
			BindingSourceReference& out) const override
		{
			out = {};
			const auto* entry = EntryAt(_root, index);
			if (!entry || !entry->Item) return false;
			out = entry->Item;
			return true;
		}
		EventConnection SubscribeChanged(ChangedHandler) override { return {}; }
		bool TryGetItemOccurrenceIdentity(
			size_t index, size_t& result) const noexcept override
		{
			result = 0;
			const auto* entry = _hasOccurrenceIdentities
				? EntryAt(_root, index) : nullptr;
			if (!entry || entry->OccurrenceIdentity == 0) return false;
			result = entry->OccurrenceIdentity;
			return true;
		}
		bool HasOccurrenceIdentities() const noexcept
		{
			return _hasOccurrenceIdentities;
		}
		DataTypeToken GetItemTypeToken() const noexcept override
		{
			return _itemTypeToken;
		}
#if CUI_ENABLE_DYNAMIC_XAML
		const std::wstring& ItemTypeName() const noexcept override
		{
			return _itemTypeName;
		}
#endif
		const std::vector<BindingListGroup>& Groups() const noexcept override
		{
			return _groups;
		}
		EventConnection SubscribeGroupsChanged(
			GroupsChangedHandler) override { return {}; }
		bool CanApply(
			const CollectionChangedEventArgs& change,
			size_t actualNewCount,
			DataTypeToken itemTypeToken) const noexcept
		{
			if (change.Action == CollectionChangeAction::Reset
				|| change.OldSize != Count()
				|| change.NewSize != actualNewCount
				|| itemTypeToken != _itemTypeToken) return false;
			switch (change.Action)
			{
			case CollectionChangeAction::Add:
				return change.NewIndex <= change.OldSize
					&& change.OldCount == 0 && change.NewCount > 0
					&& change.OldSize + change.NewCount == change.NewSize;
			case CollectionChangeAction::Remove:
				return change.OldIndex < change.OldSize
					&& change.OldCount > 0
					&& change.OldIndex + change.OldCount <= change.OldSize
					&& change.NewCount == 0
					&& change.OldSize - change.OldCount == change.NewSize;
			case CollectionChangeAction::Replace:
				return change.OldIndex == change.NewIndex
					&& change.OldCount == change.NewCount
					&& change.OldCount > 0
					&& change.OldIndex + change.OldCount <= change.OldSize
					&& change.OldSize == change.NewSize;
			case CollectionChangeAction::Move:
				return change.OldCount > 0
					&& change.OldCount == change.NewCount
					&& change.OldSize == change.NewSize
					&& change.OldIndex <= change.OldSize
					&& change.OldCount <= change.OldSize - change.OldIndex
					&& change.NewIndex <= change.NewSize - change.NewCount;
			case CollectionChangeAction::Swap:
				return change.OldCount == 1 && change.NewCount == 1
					&& change.OldSize == change.NewSize
					&& change.OldIndex < change.OldSize
					&& change.NewIndex < change.NewSize;
			default:
				return false;
			}
		}
		std::shared_ptr<MaterializedItemsSourceSnapshot> WithChange(
			const CollectionChangedEventArgs& change,
			std::vector<BindingSourceReference> changedItems,
			std::vector<size_t> changedIdentities,
			std::vector<BindingListGroup> groups) const
		{
			std::vector<Entry> entries;
			entries.reserve(changedItems.size());
			for (size_t index = 0; index < changedItems.size(); ++index)
				entries.push_back({ std::move(changedItems[index]),
					_hasOccurrenceIdentities ? changedIdentities[index] : 0 });
			Root root = _root;
			switch (change.Action)
			{
			case CollectionChangeAction::Add:
			{
				auto [before, after] = Split(root, change.NewIndex);
				root = Merge(Merge(before, Build(std::move(entries))), after);
				break;
			}
			case CollectionChangeAction::Remove:
			{
				auto [before, tail] = Split(root, change.OldIndex);
				auto [removed, after] = Split(tail, change.OldCount);
				(void)removed;
				root = Merge(before, after);
				break;
			}
			case CollectionChangeAction::Replace:
			{
				auto [before, tail] = Split(root, change.OldIndex);
				auto [removed, after] = Split(tail, change.OldCount);
				(void)removed;
				root = Merge(Merge(before, Build(std::move(entries))), after);
				break;
			}
			case CollectionChangeAction::Move:
			{
				auto [before, tail] = Split(root, change.OldIndex);
				auto [moved, after] = Split(tail, change.OldCount);
				auto remaining = Merge(before, after);
				auto [destinationBefore, destinationAfter] =
					Split(remaining, change.NewIndex);
				root = Merge(Merge(destinationBefore, moved), destinationAfter);
				break;
			}
			case CollectionChangeAction::Swap:
			{
				if (change.OldIndex == change.NewIndex) break;
				const size_t lower = (std::min)(
					change.OldIndex, change.NewIndex);
				const size_t upper = (std::max)(
					change.OldIndex, change.NewIndex);
				auto [prefix, lowerTail] = Split(root, lower);
				auto [lowerItem, middleTail] = Split(lowerTail, 1);
				auto [middle, upperTail] = Split(
					middleTail, upper - lower - 1);
				auto [upperItem, suffix] = Split(upperTail, 1);
				root = Merge(Merge(Merge(Merge(
					prefix, upperItem), middle), lowerItem), suffix);
				break;
			}
			default:
				break;
			}
			if (NodeCount(root) != change.NewSize)
				throw std::logic_error(
					"ItemsControl persistent snapshot size mismatch");
			return std::shared_ptr<MaterializedItemsSourceSnapshot>(
				new MaterializedItemsSourceSnapshot(
					_itemTypeToken,
#if CUI_ENABLE_DYNAMIC_XAML
					_itemTypeName,
#endif
					std::move(root), _hasOccurrenceIdentities,
					std::move(groups)));
		}

	private:
		MaterializedItemsSourceSnapshot(
			DataTypeToken itemTypeToken,
#if CUI_ENABLE_DYNAMIC_XAML
			std::wstring itemTypeName,
#endif
			Root root,
			bool hasOccurrenceIdentities,
			std::vector<BindingListGroup> groups)
			: _itemTypeToken(itemTypeToken),
#if CUI_ENABLE_DYNAMIC_XAML
			  _itemTypeName(std::move(itemTypeName)),
#endif
			  _root(std::move(root)),
			  _hasOccurrenceIdentities(hasOccurrenceIdentities),
			  _groups(std::move(groups)) {}

		static size_t NodeCount(const Root& node) noexcept
		{
			return node ? node->Count : 0;
		}

		static uint64_t NextPriority() noexcept
		{
			static std::atomic_uint64_t state{ 0x243f6a8885a308d3ULL };
			uint64_t value = state.fetch_add(
				0x9e3779b97f4a7c15ULL, std::memory_order_relaxed);
			value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
			value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
			return value ^ (value >> 31);
		}

		static Root MakeNode(
			Root left,
			std::shared_ptr<const std::vector<Entry>> page,
			Root right,
			uint64_t priority)
		{
			if (!page || page->empty())
				throw std::logic_error(
					"ItemsControl persistent snapshot page is empty");
			auto result = std::make_shared<Node>();
			result->Left = std::move(left);
			result->Page = std::move(page);
			result->Right = std::move(right);
			result->Priority = priority;
			result->Count = NodeCount(result->Left)
				+ result->Page->size() + NodeCount(result->Right);
			return result;
		}

		static Root Merge(const Root& left, const Root& right)
		{
			if (!left) return right;
			if (!right) return left;
			if (left->Priority <= right->Priority)
				return MakeNode(left->Left, left->Page,
					Merge(left->Right, right), left->Priority);
			return MakeNode(Merge(left, right->Left), right->Page,
				right->Right, right->Priority);
		}

		static Root Leaf(std::vector<Entry> entries)
		{
			if (entries.empty()) return {};
			return MakeNode({},
				std::make_shared<const std::vector<Entry>>(
					std::move(entries)), {}, NextPriority());
		}

		static Root Build(std::vector<Entry> entries)
		{
			Root root;
			for (size_t first = 0; first < entries.size();
				first += PageCapacity)
			{
				const size_t last = (std::min)(
					entries.size(), first + PageCapacity);
				std::vector<Entry> page;
				page.reserve(last - first);
				for (size_t index = first; index < last; ++index)
					page.push_back(std::move(entries[index]));
				root = Merge(root, Leaf(std::move(page)));
			}
			return root;
		}

		static std::pair<Root, Root> Split(
			const Root& root, size_t index)
		{
			if (!root)
			{
				if (index != 0)
					throw std::logic_error(
						"ItemsControl persistent snapshot split is outside range");
				return {};
			}
			const size_t leftCount = NodeCount(root->Left);
			const size_t pageCount = root->Page->size();
			if (index < leftCount)
			{
				auto [before, after] = Split(root->Left, index);
				return { before, MakeNode(after, root->Page,
					root->Right, root->Priority) };
			}
			if (index > leftCount + pageCount)
			{
				auto [before, after] = Split(
					root->Right, index - leftCount - pageCount);
				return { MakeNode(root->Left, root->Page,
					before, root->Priority), after };
			}
			if (index == leftCount)
				return { root->Left, MakeNode({}, root->Page,
					root->Right, root->Priority) };
			if (index == leftCount + pageCount)
				return { MakeNode(root->Left, root->Page,
					{}, root->Priority), root->Right };

			const size_t pageOffset = index - leftCount;
			std::vector<Entry> beforeEntries(
				root->Page->begin(), root->Page->begin() + pageOffset);
			std::vector<Entry> afterEntries(
				root->Page->begin() + pageOffset, root->Page->end());
			return { Merge(root->Left, Leaf(std::move(beforeEntries))),
				Merge(Leaf(std::move(afterEntries)), root->Right) };
		}

		static const Entry* EntryAt(const Root& root, size_t index) noexcept
		{
			const Node* current = root.get();
			while (current)
			{
				const size_t leftCount = NodeCount(current->Left);
				if (index < leftCount)
				{
					current = current->Left.get();
					continue;
				}
				index -= leftCount;
				if (index < current->Page->size())
					return &(*current->Page)[index];
				index -= current->Page->size();
				current = current->Right.get();
			}
			return nullptr;
		}

		DataTypeToken _itemTypeToken;
#if CUI_ENABLE_DYNAMIC_XAML
		std::wstring _itemTypeName;
#endif
		Root _root;
		bool _hasOccurrenceIdentities = false;
		std::vector<BindingListGroup> _groups;
	};

	bool TryCaptureItemsSourceSnapshot(
		const BindingListReference& source,
		BindingListReference& output,
		std::wstring& error)
	{
		output = {};
		error.clear();
		if (!source) return true;
		// Immutable random-access sources already are transaction snapshots. A
		// CollectionView may also expose an immutable snapshot of its current
		// pass-through projection. Retaining that version is O(1) and is the key
		// difference between container virtualization and true million-row data
		// virtualization: do not copy every record merely to preserve rollback.
		if (dynamic_cast<const IBindingListStableSnapshot*>(source.Get()))
		{
			output = source;
			return true;
		}
		if (const auto* provider =
			dynamic_cast<const IBindingListSnapshotProvider*>(source.Get()))
		{
			BindingListReference stable;
			if (provider->TryGetStableSnapshot(stable))
			{
				if (!stable
					|| !dynamic_cast<const IBindingListStableSnapshot*>(
						stable.Get())
					|| stable.Get()->Count() != source.Get()->Count()
					|| stable.Get()->GetItemTypeToken()
						!= source.Get()->GetItemTypeToken())
				{
					error = L"ItemsSource 稳定快照合同无效。";
					return false;
				}
				output = std::move(stable);
				return true;
			}
		}
		std::vector<BindingSourceReference> items;
		items.reserve(source.Get()->Count());
		for (size_t index = 0; index < source.Get()->Count(); ++index)
		{
			BindingSourceReference item;
			if (!source.Get()->TryGetItem(index, item) || !item)
			{
				error = L"ItemsSource 无法读取索引 "
					+ std::to_wstring(index) + L"。";
				return false;
			}
			items.push_back(std::move(item));
		}
		std::vector<size_t> occurrenceIdentities;
		const auto* identityView =
			dynamic_cast<const IBindingListOccurrenceIdentity*>(source.Get());
		bool hasOccurrenceIdentities = identityView != nullptr;
		if (identityView)
		{
			occurrenceIdentities.resize(items.size());
			for (size_t index = 0; index < items.size(); ++index)
			{
				if (identityView->TryGetItemOccurrenceIdentity(
					index, occurrenceIdentities[index])) continue;
				occurrenceIdentities.clear();
				hasOccurrenceIdentities = false;
				break;
			}
		}
		std::vector<BindingListGroup> groups;
		if (const auto* grouped = dynamic_cast<const IBindingListGroupView*>(
			source.Get()))
			groups = grouped->Groups();
		output = BindingListReference(
			std::make_shared<MaterializedItemsSourceSnapshot>(
				source.Get()->GetItemTypeToken(),
#if CUI_ENABLE_DYNAMIC_XAML
				cui::design::AuthoredBindingListItemTypeName(*source.Get()),
#endif
				std::move(items), std::move(occurrenceIdentities),
				hasOccurrenceIdentities, std::move(groups)));
		return true;
	}

	struct PreparedMaterializedSnapshotChange final
	{
		BindingListReference Replacement;
		std::vector<BindingSourceReference> ChangedItems;
		std::vector<size_t> ChangedIdentities;
		std::vector<BindingListGroup> Groups;
		bool Replace = false;
	};

	bool TryPrepareMaterializedSnapshotChange(
		const BindingListReference& source,
		const BindingListReference& materializedSnapshot,
		const CollectionChangedEventArgs& change,
		PreparedMaterializedSnapshotChange& output,
		std::wstring& error)
	{
		output = {};
		error.clear();
		auto* snapshot = materializedSnapshot
			? dynamic_cast<MaterializedItemsSourceSnapshot*>(
				materializedSnapshot.Get()) : nullptr;
		if (!source || !snapshot || !snapshot->CanApply(
			change, source.Get()->Count(), source.Get()->GetItemTypeToken()))
		{
			output.Replace = true;
			return TryCaptureItemsSourceSnapshot(
				source, output.Replacement, error);
		}
		const auto* identityView =
			dynamic_cast<const IBindingListOccurrenceIdentity*>(source.Get());
		if ((identityView != nullptr) != snapshot->HasOccurrenceIdentities())
		{
			output.Replace = true;
			return TryCaptureItemsSourceSnapshot(
				source, output.Replacement, error);
		}

		if (const auto* grouped = dynamic_cast<const IBindingListGroupView*>(
			source.Get()))
			output.Groups = grouped->Groups();
		if (change.Action == CollectionChangeAction::Add
			|| change.Action == CollectionChangeAction::Replace)
		{
			output.ChangedItems.reserve(change.NewCount);
			if (identityView)
				output.ChangedIdentities.reserve(change.NewCount);
			for (size_t offset = 0; offset < change.NewCount; ++offset)
			{
				BindingSourceReference item;
				const size_t index = change.NewIndex + offset;
				if (!source.Get()->TryGetItem(index, item) || !item)
				{
					error = L"ItemsSource 无法读取索引 "
						+ std::to_wstring(index) + L"。";
					return false;
				}
				output.ChangedItems.push_back(std::move(item));
				if (identityView)
				{
					size_t identity = 0;
					if (!identityView->TryGetItemOccurrenceIdentity(
						index, identity))
					{
						error = L"ItemsSource 无法读取索引 "
							+ std::to_wstring(index) + L" 的稳定身份。";
						return false;
					}
					output.ChangedIdentities.push_back(identity);
				}
			}
		}
		// Build the immutable candidate root before any visual mutation. Commit is
		// then one noexcept BindingListReference move; the retained old root remains
		// the exact rollback version if container preparation or a derived hook fails.
		output.Replacement = BindingListReference(snapshot->WithChange(
			change,
			std::move(output.ChangedItems),
			std::move(output.ChangedIdentities),
			std::move(output.Groups)));
		output.Replace = true;
		return true;
	}

	template<typename TValue>
	DependencyPropertyOptions<ItemsControl, TValue> DataOptions(
		TValue defaultValue
		CUI_DESIGN_METADATA_ARGUMENTS(
			int order,
			DependencyPropertyPersistence persistence =
				DependencyPropertyPersistence::Metadata))
	{
		DependencyPropertyOptions<ItemsControl, TValue> options;
		options.DefaultValue = std::move(defaultValue);
		options.Flags = DependencyPropertyFlags::AffectsMeasure
			| DependencyPropertyFlags::AffectsArrange
			| DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Data";
		options.Design.CategoryOrder = 80;
		options.Design.Order = order;
		options.Design.Editor = DependencyPropertyEditorKind::Auto;
		options.Design.Persistence = persistence;
		)
		return options;
	}

	const ItemsPanelTemplate& DefaultItemsPanel() noexcept
	{
		static const ItemsPanelTemplate value{};
		return value;
	}

	bool IsFiniteNonNegative(float value) noexcept
	{
		return std::isfinite(value) && value >= 0.0f;
	}

	bool TextEquals(
		const std::wstring& left,
		const std::wstring& right,
		bool caseSensitive) noexcept
	{
		if (left.size() != right.size()) return false;
		if (caseSensitive) return left == right;
		return std::equal(
			left.begin(), left.end(), right.begin(),
			[](wchar_t leftCharacter, wchar_t rightCharacter)
			{
				return std::towlower(leftCharacter)
					== std::towlower(rightCharacter);
			});
	}

	bool TextStartsWith(
		const std::wstring& value,
		const std::wstring& prefix,
		bool caseSensitive) noexcept
	{
		if (prefix.empty() || prefix.size() > value.size()) return false;
		if (caseSensitive)
			return std::equal(prefix.begin(), prefix.end(), value.begin());
		return std::equal(
			prefix.begin(), prefix.end(), value.begin(),
			[](wchar_t prefixCharacter, wchar_t valueCharacter)
			{
				return std::towlower(prefixCharacter)
					== std::towlower(valueCharacter);
			});
	}

	// Internal estimate used only to seed virtualized grouped extents. It is not
	// an authored GroupStyle member; realized headers measure from their template.
	constexpr float VirtualizedGroupHeaderEstimate = 24.0f;

	double SaturatingItemCoordinate(
		size_t count, float itemExtent) noexcept
	{
		if (count == 0 || !std::isfinite(itemExtent)
			|| itemExtent <= 0.0f) return 0.0;
		const double extent = static_cast<double>(itemExtent);
		const double maximum = (std::numeric_limits<double>::max)();
		const double logicalCount = static_cast<double>(count);
		return logicalCount > maximum / extent
			? maximum : logicalCount * extent;
	}

	double SaturatingDipAdd(double left, double right) noexcept
	{
		const double maximum = (std::numeric_limits<double>::max)();
		if (std::isnan(left) || left <= 0.0) left = 0.0;
		if (std::isnan(right) || right <= 0.0) right = 0.0;
		if (!std::isfinite(left) || !std::isfinite(right)
			|| right > maximum - left) return maximum;
		return left + right;
	}

	double SaturatingSignedAdd(double left, double right) noexcept
	{
		const double maximum = (std::numeric_limits<double>::max)();
		if (std::isnan(left)) left = 0.0;
		if (std::isnan(right)) right = 0.0;
		if (right > 0.0 && left > maximum - right) return maximum;
		if (right < 0.0 && left < -maximum - right) return -maximum;
		return left + right;
	}

	double SaturatingDipAdjust(double value, double adjustment) noexcept
	{
		const double maximum = (std::numeric_limits<double>::max)();
		if (std::isnan(value) || value <= 0.0) value = 0.0;
		if (std::isnan(adjustment)) adjustment = 0.0;
		if (adjustment > 0.0 && value > maximum - adjustment)
			return maximum;
		if (adjustment < 0.0 && adjustment < -value) return 0.0;
		return value + adjustment;
	}

	float SaturateLayoutDip(double value) noexcept
	{
		if (std::isnan(value)) return 0.0f;
		const double limit = static_cast<double>(
			(std::numeric_limits<float>::max)());
		return static_cast<float>((std::clamp)(value, -limit, limit));
	}

	bool IsValidItemsPanel(const ItemsPanelTemplate& value) noexcept
	{
		if (!IsFiniteNonNegative(value.ItemWidth)
			|| !IsFiniteNonNegative(value.ItemHeight)
			|| !IsFiniteNonNegative(value.CacheLength)) return false;
		if (value.Kind == ItemsPanelKind::VirtualizingStack)
			return value.Orientation == Orientation::Vertical
				&& value.ItemHeight > 0.0f;
		return true;
	}

	struct VirtualGroupHeaderMetadata final
	{
		size_t ItemIndex = 0;
		size_t HeaderCount = 0;
		size_t HeadersBefore = 0;
		size_t HeadersAfter = 0;
		double ItemTop = 0.0;
		double ItemEnd = 0.0;
	};

	/**
	 * Read-only O(1) view over one contiguous group range.
	 *
	 * Group header contexts can outlive the caller that prepared them, so retain
	 * the source strongly.  The old implementation copied every member into an
	 * ObservableBindingList while realizing the header; a single header in a
	 * million-row view could consequently fetch hundreds of thousands of items.
	 */
	class ReadOnlyBindingListSlice final
		: public IBindingList,
		  public IBindingListOccurrenceIdentity,
		  public IBindingListOccurrenceLookup
	{
	public:
		ReadOnlyBindingListSlice(
			BindingListReference source, size_t start, size_t count)
			: _source(std::move(source)), _start(start), _count(count) {}

		size_t Count() const noexcept override { return _count; }
		bool TryGetItem(
			size_t index, BindingSourceReference& out) const override
		{
			out = {};
			if (!_source || index >= _count
				|| index > (std::numeric_limits<size_t>::max)() - _start)
				return false;
			return _source.Get()->TryGetItem(_start + index, out);
		}
		EventConnection SubscribeChanged(ChangedHandler) override { return {}; }
		bool TryGetItemOccurrenceIdentity(
			size_t index, size_t& result) const noexcept override
		{
			result = 0;
			if (!_source || index >= _count
				|| index > (std::numeric_limits<size_t>::max)() - _start)
				return false;
			const auto* identities = dynamic_cast<
				const IBindingListOccurrenceIdentity*>(_source.Get());
			return identities && identities->TryGetItemOccurrenceIdentity(
				_start + index, result);
		}
		bool TryGetItemIndexByOccurrenceIdentity(
			size_t identity, size_t& index) const noexcept override
		{
			index = 0;
			if (!_source) return false;
			const auto* lookup = dynamic_cast<
				const IBindingListOccurrenceLookup*>(_source.Get());
			size_t sourceIndex = 0;
			if (!lookup || !lookup->TryGetItemIndexByOccurrenceIdentity(
				identity, sourceIndex)
				|| sourceIndex < _start
				|| sourceIndex - _start >= _count) return false;
			index = sourceIndex - _start;
			return true;
		}
		bool IsItemIndexByOccurrenceIdentityLookupBounded()
			const noexcept override
		{
			const auto* lookup = _source ? dynamic_cast<
				const IBindingListOccurrenceLookup*>(_source.Get()) : nullptr;
			return lookup
				&& lookup->IsItemIndexByOccurrenceIdentityLookupBounded();
		}
#if CUI_ENABLE_DYNAMIC_XAML
		const std::wstring& ItemTypeName() const noexcept override
		{
			static const std::wstring empty;
			return _source ? _source.Get()->ItemTypeName() : empty;
		}
#endif
		DataTypeToken GetItemTypeToken() const noexcept override
		{
			return _source ? _source.Get()->GetItemTypeToken() : DataTypeToken{};
		}

	private:
		BindingListReference _source;
		size_t _start = 0;
		size_t _count = 0;
	};

	template<typename TKey>
	void StableRadixSortGroupOrder(
		std::vector<size_t>& order,
		std::vector<size_t>& scratch,
		const std::vector<BindingListGroup>& groups,
		TKey key)
	{
		if (order.size() < 2) return;
		// IBindingListGroupView is extensible, so do not assume an implementation
		// publishes flattened groups in hierarchy order. Sort only lightweight
		// indices, then move each group once after both fixed-width key passes.
		constexpr size_t bucketCount = 256;
		for (size_t byte = 0; byte < sizeof(size_t); ++byte)
		{
			std::array<size_t, bucketCount> offsets{};
			const size_t shift = byte * 8;
			for (const size_t index : order)
				++offsets[(key(groups[index]) >> shift) & 0xffu];
			size_t next = 0;
			for (auto& offset : offsets)
			{
				const size_t count = offset;
				offset = next;
				next += count;
			}
			for (const size_t index : order)
			{
				const size_t bucket = (key(groups[index]) >> shift) & 0xffu;
				scratch[offsets[bucket]++] = index;
			}
			order.swap(scratch);
		}
	}

	class VirtualizingItemsHost;

	class VirtualizingStackLayoutEngine final : public LayoutEngine
	{
	public:
		explicit VirtualizingStackLayoutEngine(VirtualizingItemsHost& owner)
			: _owner(owner) {}
		cui::core::Size Measure(
			LayoutContext& context,
			const cui::core::Constraints& available) override;
		void Arrange(LayoutContext& context, cui::core::Rect finalRect) override;

	private:
		VirtualizingItemsHost& _owner;
	};

	class VirtualizingItemsHost final
		: public Panel,
		  public cui::framework::ILogicalScrollContent
	{
	public:
		VirtualizingItemsHost()
		{
			SetLayoutEngine(new VirtualizingStackLayoutEngine(*this));
			(void)TrySetPropertyValue(
				Control::VerticalAlignmentProperty(),
				BindingValue(VerticalAlignment::Top),
				DependencyPropertyValueSource::Template);
		}

		void SetConfiguration(
			size_t itemCount,
			float itemHeight,
			std::span<const size_t> groupHeaderStarts,
			size_t groupHeaderMetadataRevision,
			float headerHeight,
			std::span<const VirtualizedItemExtentOverride> itemExtentOverrides,
			size_t itemExtentOverrideRevision)
		{
			const bool itemCountChanged = _itemCount != itemCount;
			const bool extentChanged = _itemHeight != itemHeight
				|| _headerHeight != headerHeight;
			const bool metadataChanged = _groupHeaderMetadataRevision
				!= groupHeaderMetadataRevision;
			const bool itemOverridesChanged = itemCountChanged || extentChanged
				|| _itemExtentOverrideRevision != itemExtentOverrideRevision;
			if (!itemCountChanged && !extentChanged && !metadataChanged
				&& !itemOverridesChanged) return;
			if (metadataChanged)
			{
				std::vector<VirtualGroupHeaderMetadata> groupHeaders;
				groupHeaders.reserve(groupHeaderStarts.size() / 2);
				for (size_t offset = 0;
					offset + 1 < groupHeaderStarts.size(); offset += 2)
					groupHeaders.push_back({
						groupHeaderStarts[offset],
						groupHeaderStarts[offset + 1] });
				_groupHeaders = std::move(groupHeaders);
				_groupHeaderMetadataRevision = groupHeaderMetadataRevision;
			}
			_itemCount = itemCount;
			_itemHeight = itemHeight;
			_headerHeight = headerHeight;
			if (itemOverridesChanged)
			{
				_persistentItemExtentDeltas.clear();
				_persistentItemExtentDeltas.reserve(itemExtentOverrides.size());
				for (const auto& item : itemExtentOverrides)
				{
					if (item.ItemIndex >= _itemCount
						|| !std::isfinite(item.Extent) || item.Extent < 0.0)
						continue;
					const double delta = item.Extent
						- static_cast<double>(_itemHeight);
					if (std::abs(delta) <= 0.0001) continue;
					_persistentItemExtentDeltas.push_back({
						item.ItemIndex, delta, 0.0 });
				}
				std::sort(_persistentItemExtentDeltas.begin(),
					_persistentItemExtentDeltas.end(),
					[](const auto& left, const auto& right)
					{ return left.ItemIndex < right.ItemIndex; });
				// A derived provider should already publish a unique projection, but
				// normalize defensively so one index can never contribute twice.
				size_t write = 0;
				for (size_t read = 0;
					read < _persistentItemExtentDeltas.size(); ++read)
				{
					if (write > 0 && _persistentItemExtentDeltas[write - 1].ItemIndex
						== _persistentItemExtentDeltas[read].ItemIndex)
					{
						_persistentItemExtentDeltas[write - 1].Delta =
							_persistentItemExtentDeltas[read].Delta;
						continue;
					}
					if (write != read)
						_persistentItemExtentDeltas[write] =
							_persistentItemExtentDeltas[read];
					++write;
				}
				_persistentItemExtentDeltas.resize(write);
				_persistentItemExtentTotal = 0.0;
				for (auto& item : _persistentItemExtentDeltas)
				{
					item.DeltaBefore = _persistentItemExtentTotal;
					_persistentItemExtentTotal = SaturatingSignedAdd(
						_persistentItemExtentTotal, item.Delta);
				}
				_itemExtentOverrideRevision = itemExtentOverrideRevision;
			}
			if (itemCountChanged) RefreshLayoutOriginIndex();
			size_t totalHeaders = 0;
			for (auto& entry : _groupHeaders)
			{
				entry.HeadersBefore = totalHeaders;
				if (entry.ItemIndex >= _itemCount)
				{
					entry.HeadersAfter = totalHeaders;
					entry.ItemTop = (std::numeric_limits<double>::max)();
					entry.ItemEnd = entry.ItemTop;
					continue;
				}
				entry.ItemTop = SaturatingDipAdd(
					ItemCoordinateWithoutHeaders(entry.ItemIndex),
					SaturatingItemCoordinate(totalHeaders, _headerHeight));
				const size_t remaining =
					(std::numeric_limits<size_t>::max)() - totalHeaders;
				totalHeaders += (std::min)(remaining, entry.HeaderCount);
				entry.HeadersAfter = totalHeaders;
				entry.ItemEnd = SaturatingDipAdd(
					entry.ItemTop,
					SaturatingDipAdd(
						SaturatingDipAdjust(
							static_cast<double>(_itemHeight),
							PersistentExtentAt(entry.ItemIndex)),
						SaturatingItemCoordinate(
							entry.HeaderCount, _headerHeight)));
			}
			_totalHeaderCount = totalHeaders;
			_contentHeight = SaturatingDipAdd(
				ItemCoordinateWithoutHeaders(_itemCount),
				SaturatingItemCoordinate(_totalHeaderCount, _headerHeight));
			RebuildMeasuredItemExtents();
			if (++_configurationRevision == 0) ++_configurationRevision;
			InvalidateLayout();
		}
		void SetLogicalExtentWidth(double value) noexcept
		{
			if (!std::isfinite(value) || value < 0.0) value = 0.0;
			if (std::abs(_logicalExtentWidth - value) <= 0.0001) return;
			_logicalExtentWidth = value;
			InvalidateLayout();
		}
		void RegisterItem(Control* control, size_t index)
		{
			if (!control) return;
			const auto found = _indices.find(control);
			if (found != _indices.end())
			{
				if (found->second == index) return;
				const bool movedOrigin = found->second == _layoutOriginIndex;
				found->second = index;
				if (movedOrigin) RefreshLayoutOriginIndex();
				else if (index < _layoutOriginIndex)
					_layoutOriginIndex = index;
			}
			else
			{
				_indices.emplace(control, index);
				if (index < _layoutOriginIndex || _indices.size() == 1)
					_layoutOriginIndex = index;
			}
			RebuildMeasuredItemExtents();
			InvalidateLayout();
		}
		void UnregisterItem(Control* control)
		{
			const auto found = _indices.find(control);
			if (found == _indices.end()) return;
			const bool removedOrigin = found->second == _layoutOriginIndex;
			_measuredItemExtents.erase(control);
			_indices.erase(found);
			if (removedOrigin) RefreshLayoutOriginIndex();
			RebuildMeasuredItemExtents();
			InvalidateLayout();
		}
		void SynchronizeAuthoredItems(
			std::span<Control* const> items)
		{
			std::unordered_map<Control*, size_t> next;
			next.reserve(items.size());
			for (size_t index = 0; index < items.size(); ++index)
				if (items[index]) next.emplace(items[index], index);
			if (_indices == next) return;
			_indices = std::move(next);
			for (auto iterator = _measuredItemExtents.begin();
				iterator != _measuredItemExtents.end();)
			{
				if (_indices.contains(iterator->first)) ++iterator;
				else iterator = _measuredItemExtents.erase(iterator);
			}
			RefreshLayoutOriginIndex();
			RebuildMeasuredItemExtents();
			InvalidateLayout();
		}
		size_t IndexOf(const Control* control) const noexcept
		{
			const auto found = _indices.find(const_cast<Control*>(control));
			return found == _indices.end()
				? (std::numeric_limits<size_t>::max)() : found->second;
		}
		float ItemHeight() const noexcept { return _itemHeight; }
		double ItemTop(size_t index) const noexcept
		{
			if (index >= _itemCount) return ContentHeight();
			return SaturatingDipAdd(
				BaseItemTop(index), MeasuredExtentBefore(index));
		}
		double ItemExtent(size_t index) const noexcept
		{
			if (index >= _itemCount) return 0.0;
			double extent = BaseItemExtent(index);
			if (const auto measured = _itemExtentDeltas.find(index);
				measured != _itemExtentDeltas.end())
				extent = SaturatingDipAdd(extent, measured->second);
			return extent;
		}
		bool SetMeasuredItemExtent(
			Control* control, size_t index, double extent) noexcept
		{
			const auto registered = control ? _indices.find(control) : _indices.end();
			if (registered == _indices.end() || registered->second != index)
				return false;
			if (!std::isfinite(extent) || extent < 0.0) extent = 0.0;
			const double baseExtent = BaseItemExtent(index);
			const bool needsOverride = extent > baseExtent + 0.0001;
			const auto previous = _measuredItemExtents.find(control);
			if (!needsOverride)
			{
				if (previous == _measuredItemExtents.end()) return false;
				_measuredItemExtents.erase(previous);
			}
			else
			{
				if (previous != _measuredItemExtents.end()
					&& std::abs(previous->second - extent) <= 0.0001)
					return false;
				_measuredItemExtents[control] = extent;
			}
			RebuildMeasuredItemExtents();
			if (++_configurationRevision == 0) ++_configurationRevision;
			return true;
		}
		bool UpdatePersistentItemExtent(
			size_t index, double extent, size_t revision) noexcept
		{
			if (index >= _itemCount || !std::isfinite(extent) || extent < 0.0)
				return false;
			const double nextDelta = extent - static_cast<double>(_itemHeight);
			auto found = std::lower_bound(
				_persistentItemExtentDeltas.begin(),
				_persistentItemExtentDeltas.end(), index,
				[](const PersistentItemExtentDelta& item, size_t itemIndex)
				{ return item.ItemIndex < itemIndex; });
			const double previousDelta = found != _persistentItemExtentDeltas.end()
				&& found->ItemIndex == index ? found->Delta : 0.0;
			if (std::abs(previousDelta - nextDelta) <= 0.0001)
			{
				_itemExtentOverrideRevision = revision;
				return true;
			}
			const size_t position = static_cast<size_t>(std::distance(
				_persistentItemExtentDeltas.begin(), found));
			if (std::abs(nextDelta) <= 0.0001)
			{
				if (found != _persistentItemExtentDeltas.end()
					&& found->ItemIndex == index)
					_persistentItemExtentDeltas.erase(found);
			}
			else if (found != _persistentItemExtentDeltas.end()
				&& found->ItemIndex == index)
				found->Delta = nextDelta;
			else
				_persistentItemExtentDeltas.insert(found,
					PersistentItemExtentDelta{ index, nextDelta, 0.0 });

			const double change = nextDelta - previousDelta;
			_persistentItemExtentTotal = SaturatingSignedAdd(
				_persistentItemExtentTotal, change);
			_contentHeight = SaturatingDipAdjust(_contentHeight, change);
			double before = position == 0 ? 0.0
				: SaturatingSignedAdd(
					_persistentItemExtentDeltas[position - 1].DeltaBefore,
					_persistentItemExtentDeltas[position - 1].Delta);
			for (size_t item = position;
				item < _persistentItemExtentDeltas.size(); ++item)
			{
				_persistentItemExtentDeltas[item].DeltaBefore = before;
				before = SaturatingSignedAdd(
					before, _persistentItemExtentDeltas[item].Delta);
			}
			for (auto& header : _groupHeaders)
			{
				if (header.ItemIndex > index)
				{
					header.ItemTop = SaturatingDipAdjust(header.ItemTop, change);
					header.ItemEnd = SaturatingDipAdjust(header.ItemEnd, change);
				}
				else if (header.ItemIndex == index)
					header.ItemEnd = SaturatingDipAdjust(header.ItemEnd, change);
			}
			_itemExtentOverrideRevision = revision;
			RebuildMeasuredItemExtents();
			if (++_configurationRevision == 0) ++_configurationRevision;
			InvalidateLayout();
			return true;
		}
		size_t IndexAtOffset(double offset) const noexcept
		{
			if (_itemCount == 0) return 0;
			if (std::isnan(offset) || offset <= 0.0) return 0;
			const double contentHeight = ContentHeight();
			if (!std::isfinite(offset) || offset >= contentHeight)
				return _itemCount - 1;
			if (!_itemExtentDeltas.empty())
			{
				size_t first = 0;
				size_t last = _itemCount;
				while (first + 1 < last)
				{
					const size_t middle = first + (last - first) / 2;
					if (offset < ItemTop(middle)) last = middle;
					else first = middle;
				}
				return first;
			}
			if (_groupHeaders.empty())
			{
				if (_itemHeight <= 0.0f) return 0;
				const double quotient = std::floor(
					offset / static_cast<double>(_itemHeight));
				const double maximumIndex = static_cast<double>(
					_itemCount - 1);
				size_t index = quotient >= maximumIndex
					? _itemCount - 1 : static_cast<size_t>(quotient);
				// Multiplication and division can round on opposite sides of an
				// exact row boundary. Compare with the same logical ItemTop source
				// before returning so a boundary never selects its preceding row.
				while (index > 0 && offset < ItemTop(index)) --index;
				while (index + 1 < _itemCount
					&& offset >= ItemTop(index + 1)) ++index;
				return index;
			}
			const auto next = std::upper_bound(
				_groupHeaders.begin(), _groupHeaders.end(), offset,
				[](double value, const VirtualGroupHeaderMetadata& entry)
				{ return value < entry.ItemTop; });
			size_t segmentFirst = 0;
			size_t segmentLast = next == _groupHeaders.end()
				? _itemCount : next->ItemIndex;
			double segmentTop = 0.0;
			if (next != _groupHeaders.begin())
			{
				const auto& previous = *std::prev(next);
				if (offset < previous.ItemEnd) return previous.ItemIndex;
				segmentFirst = previous.ItemIndex + 1;
				segmentTop = previous.ItemEnd;
			}
			if (segmentFirst >= segmentLast)
				return next != _groupHeaders.end()
					? next->ItemIndex : _itemCount - 1;
			if (_itemHeight <= 0.0f) return segmentFirst;
			const double delta = offset > segmentTop
				? offset - segmentTop : 0.0;
			const double quotient = std::floor(
				delta / static_cast<double>(_itemHeight));
			const size_t segmentCount = segmentLast - segmentFirst;
			size_t index = quotient >= static_cast<double>(segmentCount - 1)
				? segmentLast - 1
				: segmentFirst + static_cast<size_t>(quotient);
			// Keep exact row boundaries coherent with ItemTop even when division and
			// multiplication round on opposite sides of the same large coordinate.
			while (index > segmentFirst && offset < ItemTop(index)) --index;
			while (index + 1 < segmentLast
				&& offset >= ItemTop(index + 1)) ++index;
			return index;
		}
		double ContentHeight() const noexcept
		{
			return SaturatingDipAdd(
				_contentHeight, _measuredItemExtentTotal);
		}
		size_t GroupHeaderMetadataCount() const noexcept
		{
			return _groupHeaders.size();
		}
		size_t ConfigurationRevision() const noexcept
		{
			return _configurationRevision;
		}
		size_t PersistentItemExtentOverrideCount() const noexcept
		{
			return _persistentItemExtentDeltas.size();
		}
		double LogicalExtentHeightDip() const noexcept override
		{
			return ContentHeight();
		}
		double LogicalExtentWidthDip() const noexcept override
		{
			return _logicalExtentWidth;
		}
		double VerticalLayoutOriginDip() const noexcept override
		{
			if (_indices.empty() || _layoutOriginIndex >= _itemCount)
				return 0.0;
			// The origin only has to keep float layout coordinates small near the
			// millionth record. Quantize it to a fixed band instead of tracking the
			// first realized row: every virtualization step would otherwise move
			// the origin, re-arrange every surviving row at a new relative offset
			// and invalidate their complete visual subtrees although nothing on
			// screen moved except the scroll translation. Within one band the
			// realized rows stay below ~40k DIP, where float spacing is ~0.004 DIP.
			constexpr double LayoutOriginBandDip = 32768.0;
			const double top = ItemTop(_layoutOriginIndex);
			if (!std::isfinite(top) || top <= LayoutOriginBandDip) return 0.0;
			return std::floor(top / LayoutOriginBandDip) * LayoutOriginBandDip;
		}
		void OnVerticalThumbDragCompleted() override
		{
			// Direct Thumb movement temporarily projects only the visible page for
			// DataGrid. Restore the authored cache once, after capture has ended.
			if (auto* owner = dynamic_cast<ItemsControl*>(GetTemplatedParent()))
				cui::framework::ItemsControlAccess::
					RestoreVirtualCacheAfterVerticalThumbDrag(*owner);
		}
		bool SetLocalLayoutInvalidation(bool value) noexcept
		{
			return std::exchange(_localLayoutInvalidation, value);
		}

	private:
		struct PersistentItemExtentDelta final
		{
			size_t ItemIndex = 0;
			double Delta = 0.0;
			double DeltaBefore = 0.0;
		};
		double PersistentExtentBefore(size_t index) const noexcept
		{
			const auto found = std::lower_bound(
				_persistentItemExtentDeltas.begin(),
				_persistentItemExtentDeltas.end(), index,
				[](const PersistentItemExtentDelta& item, size_t itemIndex)
				{ return item.ItemIndex < itemIndex; });
			return found == _persistentItemExtentDeltas.end()
				? _persistentItemExtentTotal : found->DeltaBefore;
		}
		double PersistentExtentAt(size_t index) const noexcept
		{
			const auto found = std::lower_bound(
				_persistentItemExtentDeltas.begin(),
				_persistentItemExtentDeltas.end(), index,
				[](const PersistentItemExtentDelta& item, size_t itemIndex)
				{ return item.ItemIndex < itemIndex; });
			return found != _persistentItemExtentDeltas.end()
				&& found->ItemIndex == index ? found->Delta : 0.0;
		}
		double ItemCoordinateWithoutHeaders(size_t index) const noexcept
		{
			return SaturatingDipAdjust(
				SaturatingItemCoordinate(index, _itemHeight),
				PersistentExtentBefore(index));
		}
		double BaseItemTop(size_t index) const noexcept
		{
			if (index >= _itemCount) return _contentHeight;
			if (_groupHeaders.empty())
				return ItemCoordinateWithoutHeaders(index);
			const auto next = std::lower_bound(
				_groupHeaders.begin(), _groupHeaders.end(), index,
				[](const VirtualGroupHeaderMetadata& entry, size_t itemIndex)
				{ return entry.ItemIndex < itemIndex; });
			const size_t headersBefore = next == _groupHeaders.begin()
				? size_t{ 0 } : std::prev(next)->HeadersAfter;
			return SaturatingDipAdd(
				ItemCoordinateWithoutHeaders(index),
				SaturatingItemCoordinate(headersBefore, _headerHeight));
		}
		double BaseItemExtent(size_t index) const noexcept
		{
			if (index >= _itemCount) return 0.0;
			if (_groupHeaders.empty())
				return SaturatingDipAdjust(
					static_cast<double>(_itemHeight),
					PersistentExtentAt(index));
			const auto entry = std::lower_bound(
				_groupHeaders.begin(), _groupHeaders.end(), index,
				[](const VirtualGroupHeaderMetadata& candidate, size_t itemIndex)
				{ return candidate.ItemIndex < itemIndex; });
			const size_t headerCount = entry != _groupHeaders.end()
				&& entry->ItemIndex == index ? entry->HeaderCount : 0;
			return SaturatingDipAdd(
				SaturatingDipAdjust(static_cast<double>(_itemHeight),
					PersistentExtentAt(index)),
				SaturatingItemCoordinate(headerCount, _headerHeight));
		}
		double MeasuredExtentBefore(size_t index) const noexcept
		{
			double result = 0.0;
			for (auto iterator = _itemExtentDeltas.begin();
				iterator != _itemExtentDeltas.lower_bound(index); ++iterator)
				result = SaturatingDipAdd(result, iterator->second);
			return result;
		}
		void RebuildMeasuredItemExtents() noexcept
		{
			_itemExtentDeltas.clear();
			_measuredItemExtentTotal = 0.0;
			for (auto iterator = _measuredItemExtents.begin();
				iterator != _measuredItemExtents.end();)
			{
				const auto registered = _indices.find(iterator->first);
				if (registered == _indices.end()
					|| registered->second >= _itemCount)
				{
					iterator = _measuredItemExtents.erase(iterator);
					continue;
				}
				const double delta = (std::max)(0.0,
					iterator->second - BaseItemExtent(registered->second));
				if (delta > 0.0001)
				{
					_itemExtentDeltas[registered->second] = delta;
					_measuredItemExtentTotal = SaturatingDipAdd(
						_measuredItemExtentTotal, delta);
				}
				++iterator;
			}
		}
		void RefreshLayoutOriginIndex() noexcept
		{
			_layoutOriginIndex = _itemCount;
			for (const auto& [control, index] : _indices)
			{
				(void)control;
				if (index < _layoutOriginIndex) _layoutOriginIndex = index;
			}
		}

		bool ShouldPropagateLayoutInvalidation() const noexcept override
		{
			// Realization caused solely by a changed scroll offset does not
			// change the virtual host's total extent. Keep that structural
			// mutation inside the host and commit its new children immediately.
			return !_localLayoutInvalidation;
		}

		size_t _itemCount = 0;
		float _itemHeight = 0.0f;
		float _headerHeight = 0.0f;
		std::vector<VirtualGroupHeaderMetadata> _groupHeaders;
		double _contentHeight = 0.0;
		double _persistentItemExtentTotal = 0.0;
		double _measuredItemExtentTotal = 0.0;
		double _logicalExtentWidth = 0.0;
		size_t _totalHeaderCount = 0;
		size_t _groupHeaderMetadataRevision = 0;
		size_t _itemExtentOverrideRevision = 0;
		size_t _configurationRevision = 0;
		std::vector<PersistentItemExtentDelta> _persistentItemExtentDeltas;
		std::unordered_map<Control*, size_t> _indices;
		std::unordered_map<Control*, double> _measuredItemExtents;
		std::map<size_t, double> _itemExtentDeltas;
		size_t _layoutOriginIndex = 0;
		bool _localLayoutInvalidation = false;
	};

	class GroupedItemHost;

	class GroupedItemsLayoutEngine final : public LayoutEngine
	{
	public:
		explicit GroupedItemsLayoutEngine(GroupedItemHost& owner)
			: _owner(owner) {}
		cui::core::Size Measure(
			LayoutContext& context,
			const cui::core::Constraints& available) override;
		void Arrange(
			LayoutContext& context,
			cui::core::Rect finalRect) override;

	private:
		GroupedItemHost& _owner;
	};

	class GroupedItemHost final : public Panel
	{
	public:
		GroupedItemHost(
			std::unique_ptr<Control> item,
			Control* itemLogicalParent)
			: Panel(), _itemLogicalParent(itemLogicalParent)
		{
			SetLayoutEngine(new GroupedItemsLayoutEngine(*this));
			_item = item.get();
			cui::framework::TreeAccess::AddOwnedVisualChild(
				*this, std::move(item), itemLogicalParent);
		}

		Control* Item() const noexcept { return _item; }
		size_t HeaderCount() const noexcept { return _headerCount; }
		float FixedHeaderHeight() const noexcept { return _fixedHeaderHeight; }
		float FixedItemHeight() const noexcept { return _fixedItemHeight; }
		bool ItemUsesDynamicHeight() const noexcept
		{
			auto* owner = dynamic_cast<ItemsControl*>(
				_itemLogicalParent.Get());
			return owner && _item
				&& cui::framework::ItemsControlAccess::
					UseMeasuredVirtualizedItemHeight(*owner, *_item);
		}
		std::unique_ptr<Control> TakeItem()
		{
			auto result = DetachVisualChild(_item);
			_item = nullptr;
			return result;
		}
		void SetHeaders(
			std::vector<std::unique_ptr<Control>> headers,
			std::vector<BindingSourceReference> contexts,
			float fixedHeaderHeight = 0.0f,
			float fixedItemHeight = 0.0f)
		{
			if (_changingHeaders)
				throw std::logic_error(
					"GroupedItemHost does not support reentrant header changes");
			const auto initialChildren = GetVisualChildrenView();
			if (!_item
				|| _headerCount > initialChildren.size()
				|| initialChildren.size() != _headerCount + 1
				|| initialChildren[_headerCount] != _item
				|| _contexts.size() != _headerCount)
				throw std::logic_error(
					"GroupedItemHost current header metadata is invalid");
			if (contexts.size() != headers.size()
				|| std::any_of(
					headers.begin(), headers.end(),
					[](const auto& header) { return !header; }))
				throw std::invalid_argument(
					"GroupedItemHost header visuals and contexts do not match");
			_changingHeaders = true;
			struct HeaderChangeGuard final
			{
				bool& Value;
				~HeaderChangeGuard() { Value = false; }
			} changeGuard{ _changingHeaders };

			struct PreviousChild final
			{
				ControlWeakReference Lifetime;
				ControlWeakReference LogicalParent;
				bool IsItem = false;
				size_t HeaderIndex = static_cast<size_t>(-1);
				std::unique_ptr<Control> Owner;
			};

			const ControlWeakReference previousItemLifetime(_item);
			const auto previousHeaderCount = _headerCount;
			const auto previousFixedHeaderHeight = _fixedHeaderHeight;
			const auto previousFixedItemHeight = _fixedItemHeight;
			const auto previousContexts = _contexts;
			std::vector<BindingSourceReference> restoredContextsScratch;
			restoredContextsScratch.reserve(previousHeaderCount);
			std::vector<ControlWeakReference> candidateHeaders;
			candidateHeaders.reserve(headers.size());
			std::vector<PreviousChild> previousChildren;
			previousChildren.reserve(GetVisualChildrenView().size());
			size_t visualIndex = 0;
			for (auto* child : GetVisualChildrenView())
			{
				previousChildren.push_back(PreviousChild{
					ControlWeakReference(child),
					ControlWeakReference(child
						? child->GetLogicalParent() : nullptr),
					child && child == _item,
					visualIndex < previousHeaderCount
						? visualIndex : static_cast<size_t>(-1),
					{} });
				++visualIndex;
			}

			std::exception_ptr firstNotificationError;
			std::exception_ptr transactionError;
			auto rememberFirst = [&](std::exception_ptr error)
			{
				if (error && !firstNotificationError)
					firstNotificationError = std::move(error);
			};
			auto fail = [&](const char* message)
			{
				if (!transactionError)
					transactionError = std::make_exception_ptr(
						std::logic_error(message));
			};
			auto isOwnedHere = [this](Control* child) noexcept
			{
				return child && child->GetVisualParent() == this
					&& IndexOfVisualChild(child) >= 0;
			};

			// Never publish a raw item while callbacks can detach, transfer or
			// destroy it. The slot is republished only from a verified final tree.
			_item = nullptr;
			_headerCount = 0;

			for (auto& entry : previousChildren)
			{
				auto* live = entry.Lifetime.Get();
				if (!live)
				{
					fail("GroupedItemHost previous child expired");
					break;
				}
				bool ownershipCommit = false;
				std::exception_ptr notificationError;
				try
				{
					entry.Owner =
						cui::framework::TreeAccess::DetachVisualChild(
							*this, live, &ownershipCommit,
							&notificationError);
				}
				catch (...)
				{
					transactionError = std::current_exception();
					break;
				}
				rememberFirst(notificationError);
				live = entry.Lifetime.Get();
				if (!entry.Owner || entry.Owner.get() != live)
				{
					fail(isOwnedHere(live)
						? "GroupedItemHost previous child detach did not commit"
						: "GroupedItemHost previous child ownership escaped");
					break;
				}
			}
			if (!transactionError && VisualChildCount() != 0)
				fail("GroupedItemHost detach left unexpected visual children");

			if (!transactionError)
			{
				for (auto& header : headers)
				{
					if (!header)
					{
						fail("GroupedItemHost header is null");
						break;
					}
					const ControlWeakReference lifetime(header.get());
					std::exception_ptr attachError;
					try
					{
						(void)cui::framework::TreeAccess::
							InsertOwnedVisualChildPreserving(
								*this, VisualChildCount(), header, this);
					}
					catch (...)
					{
						attachError = std::current_exception();
					}
					auto* live = lifetime.Get();
					const bool attached = isOwnedHere(live)
						&& live->GetLogicalParent() == this;
					if (!attached)
					{
						transactionError = attachError
							? attachError
							: std::make_exception_ptr(std::logic_error(
								"GroupedItemHost header attachment did not commit"));
						break;
					}
					candidateHeaders.push_back(lifetime);
					rememberFirst(attachError);
				}
			}

			auto itemEntry = std::find_if(
				previousChildren.begin(), previousChildren.end(),
				[](const PreviousChild& entry) { return entry.IsItem; });
			ControlWeakReference committedItem;
			if (!transactionError)
			{
				if (itemEntry == previousChildren.end() || !itemEntry->Owner)
					fail("GroupedItemHost lost its item owner");
				else
				{
					const auto itemLifetime = itemEntry->Lifetime;
					// A recycled grouped host is detached with the generated item's
					// logical parent cleared. Reapply the owning ItemsControl when the
					// item is committed into the replacement header transaction; using
					// the detached nullptr would make the subsequent host attachment
					// fail its logical ownership contract.
					auto* logicalParent = _itemLogicalParent.Get();
					std::exception_ptr attachError;
					try
					{
						(void)cui::framework::TreeAccess::
							InsertOwnedVisualChildPreserving(
								*this, VisualChildCount(),
								itemEntry->Owner, logicalParent);
					}
					catch (...)
					{
						attachError = std::current_exception();
					}
					auto* live = itemLifetime.Get();
					if (!isOwnedHere(live)
						|| live->GetLogicalParent() != logicalParent)
						transactionError = attachError
							? attachError
							: std::make_exception_ptr(std::logic_error(
								"GroupedItemHost item attachment did not commit"));
					else
					{
						committedItem = itemLifetime;
						rememberFirst(attachError);
					}
				}
			}

			if (!transactionError)
			{
				const auto children = GetVisualChildrenView();
				bool exact = children.size() == candidateHeaders.size() + 1;
				for (size_t index = 0;
					exact && index < candidateHeaders.size(); ++index)
					exact = children[index] == candidateHeaders[index].Get();
				exact = exact && children.back() == committedItem.Get();
				if (!exact)
					fail("GroupedItemHost header transaction final state is invalid");
			}

			if (!transactionError)
			{
				_item = committedItem.Get();
				_headerCount = candidateHeaders.size();
				_fixedHeaderHeight =
					(std::max)(0.0f, fixedHeaderHeight);
				_fixedItemHeight =
					(std::max)(0.0f, fixedItemHeight);
				_contexts = std::move(contexts);
				InvalidateLayout();
				if (firstNotificationError)
					std::rethrow_exception(firstNotificationError);
				return;
			}
			if (firstNotificationError)
				transactionError = firstNotificationError;

			// Remove every candidate or callback-injected visual before restoring
			// the old weak identities. Detached owners are intentionally local:
			// failed replacement content must not survive the transaction.
			for (size_t pass = 0; pass < 1024; ++pass)
			{
				Control* unexpected = nullptr;
				for (auto* child : GetVisualChildrenView())
				{
					const bool wasPrevious = std::any_of(
						previousChildren.begin(), previousChildren.end(),
						[child](const PreviousChild& entry)
						{ return entry.Lifetime.Get() == child; });
					if (child && !wasPrevious)
					{
						unexpected = child;
						break;
					}
				}
				if (!unexpected) break;
				const ControlWeakReference childReference(unexpected);
				auto* child = childReference.Get();
				if (!child || !isOwnedHere(child)) continue;
				bool ownershipCommit = false;
				std::exception_ptr ignoredNotificationError;
				try
				{
					auto discarded =
						cui::framework::TreeAccess::DetachVisualChild(
							*this, child, &ownershipCommit,
							&ignoredNotificationError);
					(void)discarded;
				}
				catch (...)
				{
					// Rollback is best-effort, but metadata below is always
					// republished from the surviving weak identities.
				}
			}

			for (auto& entry : previousChildren)
			{
				auto* live = entry.Lifetime.Get();
				if (isOwnedHere(live)) continue;
				if (!entry.Owner || entry.Owner.get() != live) continue;
				try
				{
					(void)cui::framework::TreeAccess::
						InsertOwnedVisualChildPreserving(
							*this, VisualChildCount(), entry.Owner,
							entry.LogicalParent.Get());
				}
				catch (...)
				{
					// Preserve transactionError. A post-commit restoration is
					// recognized by the final weak/parent scan below.
				}
			}

			size_t restoreIndex = 0;
			for (const auto& entry : previousChildren)
			{
				auto* live = entry.Lifetime.Get();
				if (!isOwnedHere(live)) continue;
				const int currentIndex = IndexOfVisualChild(live);
				if (currentIndex != static_cast<int>(restoreIndex))
				try
				{
					(void)MoveVisualChild(
						currentIndex, static_cast<int>(restoreIndex));
				}
				catch (...) {}
				++restoreIndex;
			}

			auto* restoredItem = previousItemLifetime.Get();
			_item = isOwnedHere(restoredItem) ? restoredItem : nullptr;
			for (const auto& entry : previousChildren)
			{
				if (entry.HeaderIndex == static_cast<size_t>(-1)
					|| entry.HeaderIndex >= previousContexts.size())
					continue;
				if (isOwnedHere(entry.Lifetime.Get()))
					restoredContextsScratch.push_back(
						previousContexts[entry.HeaderIndex]);
			}
			_headerCount = restoredContextsScratch.size();
			_contexts = std::move(restoredContextsScratch);
			_fixedHeaderHeight = previousFixedHeaderHeight;
			_fixedItemHeight = previousFixedItemHeight;
			InvalidateLayout();
			std::rethrow_exception(transactionError);
		}

	private:
		ControlWeakReference _itemLogicalParent;
		Control* _item = nullptr;
		size_t _headerCount = 0;
		float _fixedHeaderHeight = 0.0f;
		float _fixedItemHeight = 0.0f;
		std::vector<BindingSourceReference> _contexts;
		bool _changingHeaders = false;
	};

	struct GroupedLayoutItem final
	{
		Control* Child = nullptr;
		Thickness Margin{};
		cui::core::Size Desired{};
		float OuterHeight = 0.0f;
	};

	float DeflateGroupedExtent(float extent, float before, float after)
	{
		return std::isfinite(extent)
			? (std::max)(0.0f, extent - before - after)
			: cui::core::Infinity;
	}

	std::vector<GroupedLayoutItem> MeasureGroupedItems(
		GroupedItemHost& owner,
		LayoutContext& context,
		float availableWidth)
	{
		std::vector<GroupedLayoutItem> result;
		result.reserve(static_cast<size_t>((std::max)(0, context.ChildCount())));
		for (int childIndex = 0; childIndex < context.ChildCount(); ++childIndex)
		{
			auto* child = context.ChildAt(childIndex);
			if (!child || child->IsCollapsed()) continue;
			const bool isHeader = static_cast<size_t>(childIndex)
				< owner.HeaderCount();
			const auto margin = child->Margin;
			const bool dynamicItem = !isHeader
				&& owner.ItemUsesDynamicHeight();
			const float fixedOuterHeight = isHeader
				? owner.FixedHeaderHeight()
				: dynamicItem ? 0.0f : owner.FixedItemHeight();
			const float childWidth = DeflateGroupedExtent(
				availableWidth, margin.Left, margin.Right);
			const float childHeight = fixedOuterHeight > 0.0f
				? (std::max)(0.0f,
					fixedOuterHeight - margin.Top - margin.Bottom)
				: cui::core::Infinity;
			const auto desired = child->Measure(cui::core::Constraints{
				cui::core::Size{ childWidth, childHeight } });
			result.push_back(GroupedLayoutItem{
				child, margin, desired,
				fixedOuterHeight > 0.0f ? fixedOuterHeight
					: desired.height + margin.Top + margin.Bottom });
		}
		return result;
	}

	cui::core::Size GroupedItemsLayoutEngine::Measure(
		LayoutContext& context,
		const cui::core::Constraints& available)
	{
		const auto items = MeasureGroupedItems(
			_owner, context, available.Normalized().maximum.width);
		cui::core::Size desired{};
		for (const auto& item : items)
		{
			desired.width = (std::max)(desired.width,
				item.Margin.Left + item.Desired.width
					+ item.Margin.Right);
			desired.height += item.OuterHeight;
		}
		_needsLayout = false;
		return desired;
	}

	void GroupedItemsLayoutEngine::Arrange(
		LayoutContext& context,
		cui::core::Rect finalRect)
	{
		finalRect = finalRect.Normalized();
		auto items = MeasureGroupedItems(
			_owner, context, finalRect.width);
		if (_owner.FixedItemHeight() > 0.0f
			&& !_owner.ItemUsesDynamicHeight())
		{
			// The virtualizing stack owns the authoritative per-index extent. A
			// grouped host stores only the common baseline, so a sparse item extent
			// override arrives as extra (or reduced) space in finalRect. Give that
			// space to the real item instead of leaving it blank below the baseline;
			// otherwise a resized DataGridRow moves following groups while its row,
			// cells and grid line remain at the old height.
			float nonItemHeight = 0.0f;
			GroupedLayoutItem* itemLayout = nullptr;
			for (auto& item : items)
			{
				if (item.Child == _owner.Item()) itemLayout = &item;
				else nonItemHeight += item.OuterHeight;
			}
			if (itemLayout)
				itemLayout->OuterHeight = (std::max)(
					0.0f, finalRect.height - nonItemHeight);
		}
		float currentY = finalRect.y;
		for (size_t index = 0; index < items.size(); ++index)
		{
			const auto& item = items[index];
			const float availableWidth = DeflateGroupedExtent(
				finalRect.width, item.Margin.Left, item.Margin.Right);
			const float availableHeight = (std::max)(0.0f,
				item.OuterHeight - item.Margin.Top - item.Margin.Bottom);
			float width = (std::min)(item.Desired.width, availableWidth);
			float height = (std::min)(item.Desired.height, availableHeight);
			if (item.Child->HorizontalAlignment == HorizontalAlignment::Stretch)
				width = availableWidth;
			if (item.Child->VerticalAlignment == VerticalAlignment::Stretch)
				height = availableHeight;
			float x = finalRect.x + item.Margin.Left;
			if (item.Child->HorizontalAlignment == HorizontalAlignment::Center)
				x += (availableWidth - width) * 0.5f;
			else if (item.Child->HorizontalAlignment == HorizontalAlignment::Right)
				x += availableWidth - width;
			float y = currentY + item.Margin.Top;
			if (item.Child->VerticalAlignment == VerticalAlignment::Center)
				y += (availableHeight - height) * 0.5f;
			else if (item.Child->VerticalAlignment == VerticalAlignment::Bottom)
				y += availableHeight - height;
			item.Child->Arrange(cui::core::Rect{ x, y, width, height });
			currentY += item.OuterHeight;
		}
		_needsLayout = false;
	}

	cui::core::Size VirtualizingStackLayoutEngine::Measure(
		LayoutContext& context,
		const cui::core::Constraints& available)
	{
		const auto maximum = available.Normalized().maximum;
		float desiredWidth = 0.0f;
		for (int index = 0; index < context.ChildCount(); ++index)
		{
			auto* child = context.ChildAt(index);
			if (!child || child->IsCollapsed()) continue;
			const auto margin = child->Margin;
			const float width = std::isfinite(maximum.width)
				? (std::max)(0.0f, maximum.width - margin.Left - margin.Right)
				: cui::core::Infinity;
			const auto itemIndex = _owner.IndexOf(child);
			if (itemIndex == (std::numeric_limits<size_t>::max)()) continue;
			const auto* grouped = dynamic_cast<GroupedItemHost*>(child);
			auto* itemsOwner = dynamic_cast<ItemsControl*>(
				_owner.GetTemplatedParent());
			const auto* logicalItem = grouped ? grouped->Item() : child;
			const bool dynamicHeight = itemsOwner && logicalItem
				&& cui::framework::ItemsControlAccess::
					UseMeasuredVirtualizedItemHeight(
						*itemsOwner, *logicalItem);
			if (!dynamicHeight)
				(void)_owner.SetMeasuredItemExtent(child, itemIndex, 0.0);
			const float height = dynamicHeight
				? cui::core::Infinity
				: (std::max)(0.0f,
					SaturateLayoutDip(_owner.ItemExtent(itemIndex))
					- margin.Top - margin.Bottom);
			const auto desired = child->Measure(
				cui::core::Constraints{ cui::core::Size{ width, height } });
			if (dynamicHeight)
				(void)_owner.SetMeasuredItemExtent(child, itemIndex,
					static_cast<double>(desired.height)
						+ margin.Top + margin.Bottom);
			desiredWidth = (std::max)(desiredWidth,
				desired.width + margin.Left + margin.Right);
		}
		_needsLayout = false;
		return cui::core::Size{
			desiredWidth, SaturateLayoutDip(_owner.ContentHeight()) };
	}

	void VirtualizingStackLayoutEngine::Arrange(
		LayoutContext& context,
		cui::core::Rect finalRect)
	{
		finalRect = finalRect.Normalized();
		const float width = finalRect.width;
		// Core layout geometry remains float. Rebase every realized row against
		// the first cached row so neither Arrange nor the render transform has to
		// subtract two ~38-million-DIP floats near the millionth record.
		const double layoutOrigin = _owner.VerticalLayoutOriginDip();
		for (int childIndex = 0; childIndex < context.ChildCount(); ++childIndex)
		{
			auto* child = context.ChildAt(childIndex);
			if (!child || child->IsCollapsed()) continue;
			const auto itemIndex = _owner.IndexOf(child);
			if (itemIndex == (std::numeric_limits<size_t>::max)()) continue;
			const auto margin = child->Margin;
			const float cellTop = SaturateLayoutDip(
				_owner.ItemTop(itemIndex) - layoutOrigin);
			child->Arrange(cui::core::Rect{
				finalRect.x + margin.Left,
				finalRect.y + cellTop + margin.Top,
				(std::max)(0.0f, width - margin.Left - margin.Right),
				(std::max)(0.0f,
					SaturateLayoutDip(_owner.ItemExtent(itemIndex))
					- margin.Top - margin.Bottom) });
		}
		_needsLayout = false;
	}
}

struct ItemsControl::DirectVisualMutationFrame final
{
	explicit DirectVisualMutationFrame(
		ItemsControl& owner,
		Control* after)
		: Owner(owner),
		Previous(owner._activeDirectVisualMutationFrame),
		After(after),
		HasAfter(after != nullptr)
	{
		const auto before = owner.GetVisualChildrenView();
		auto* previousInfrastructure = std::find(
			before.begin(), before.end(),
			owner._controlTemplateRoot) != before.end()
			? owner._controlTemplateRoot
			: std::find(
				before.begin(), before.end(),
				owner._itemsHost) != before.end()
				? owner._itemsHost : nullptr;
		if (previousInfrastructure)
		{
			Before = previousInfrastructure;
			HasBefore = true;
		}
		owner._activeDirectVisualMutationFrame = this;
	}
	DirectVisualMutationFrame(
		const DirectVisualMutationFrame&) = delete;
	DirectVisualMutationFrame& operator=(
		const DirectVisualMutationFrame&) = delete;
	~DirectVisualMutationFrame()
	{
		Owner._activeDirectVisualMutationFrame = Previous;
	}

	ItemsControl& Owner;
	DirectVisualMutationFrame* Previous = nullptr;
	ControlWeakReference Before;
	ControlWeakReference After;
	bool HasBefore = false;
	bool HasAfter = false;
};

ItemsControl::ItemsControl()
	: Control()
{
	auto itemsHost = CreateItemsHost();
	_itemsHost = itemsHost.get();
	cui::framework::TreeAccess::SetTemplatedParent(*_itemsHost, this);
	DirectVisualMutationFrame mutation(*this, _itemsHost);
	cui::framework::TreeAccess::AddOwnedVisualChild(
		*this, std::move(itemsHost), nullptr);
	RefreshItemsScrollOwner();
}

bool ItemsControl::ValidateVisualChildCollection(
	std::span<Control* const> children,
	std::string& error) const
{
	if (_activeDirectVisualMutationFrame)
	{
		const auto& frame = *_activeDirectVisualMutationFrame;
		auto matches = [&](
			bool hasExpected,
			const ControlWeakReference& expected) noexcept
		{
			if (!hasExpected) return children.empty();
			return children.size() == 1 && children.front()
				&& expected.Get() == children.front();
		};
		if (matches(frame.HasBefore, frame.Before)
			|| matches(frame.HasAfter, frame.After))
			return true;
		error =
			"ItemsControl direct-child transaction rejected an unexpected visual";
		return false;
	}
	if (_controlTemplateRoot && children.size() == 1
		&& children.front() == _controlTemplateRoot)
		return true;
	if (!_controlTemplateRoot && _itemsHost && children.size() == 1
		&& children.front() == _itemsHost)
		return true;
	error = "ItemsControl direct children are owned by its ControlTemplate or ItemsPanelTemplate host";
	return false;
}

const DependencyProperty& ItemsControl::ItemsSourceProperty()
{
	static const auto registration = []
	{
		auto options = DataOptions(
			BindingListReference{}
			CUI_DESIGN_METADATA_ARGUMENTS(
				10, DependencyPropertyPersistence::Native));
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			ItemsControl, BindingListReference>(
				DependencyPropertyRegistrationLiteral(L"ItemsSource"),
				[](ItemsControl& target) { return target.GetItemsSource(); },
				[](ItemsControl& target, const BindingListReference& value)
				{ target.SetItemsSource(value); }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& ItemsControl::ItemTemplateProperty()
{
	static const auto registration = []
	{
		auto options = DataOptions(
			ItemTemplateReference{}
			CUI_DESIGN_METADATA_ARGUMENTS(
				20, DependencyPropertyPersistence::Native));
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			ItemsControl, ItemTemplateReference>(
				DependencyPropertyRegistrationLiteral(L"ItemTemplate"),
				[](ItemsControl& target) { return target.GetItemTemplate(); },
				[](ItemsControl& target, const ItemTemplateReference& value)
				{ target.SetItemTemplate(value); }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& ItemsControl::GroupStyleProperty()
{
	static const auto registration = []
	{
		auto options = DataOptions(
			GroupStyleReference{}
			CUI_DESIGN_METADATA_ARGUMENTS(
				25, DependencyPropertyPersistence::Native));
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			ItemsControl, GroupStyleReference>(
				DependencyPropertyRegistrationLiteral(L"GroupStyle"),
				[](ItemsControl& target) { return target.GetGroupStyle(); },
				[](ItemsControl& target, const GroupStyleReference& value)
				{ target.SetGroupStyle(value); }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& ItemsControl::ItemsPanelProperty()
{
	static const auto registration = []
	{
		auto options = DataOptions(
			ItemsPanelTemplateReference{}
			CUI_DESIGN_METADATA_ARGUMENTS(
				30, DependencyPropertyPersistence::Native));
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			ItemsControl, ItemsPanelTemplateReference>(
				DependencyPropertyRegistrationLiteral(L"ItemsPanel"),
				[](ItemsControl& target) { return target.GetItemsPanel(); },
				[](ItemsControl& target, const ItemsPanelTemplateReference& value)
				{ target.SetItemsPanel(value); }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& ItemsControl::ItemContainerStyleProperty()
{
	static const auto registration = []
	{
		auto options = DataOptions(
			std::wstring{}
			CUI_DESIGN_METADATA_ARGUMENTS(
				50, DependencyPropertyPersistence::Native));
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Browsable = false;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			ItemsControl, std::wstring>(
				DependencyPropertyRegistrationLiteral(L"ItemContainerStyle"),
				[](ItemsControl& target) { return target.GetItemContainerStyle(); },
				[](ItemsControl& target, const std::wstring& value)
				{ target.SetItemContainerStyle(value); }, {},
				std::move(options));
	}();
	return *registration;
}

void ItemsControl::RegisterDependencyProperties()
{
	Control::RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	RegisterDesignDependencyProperties();
#endif
}

const DependencyProperty& ItemsControl::IsTextSearchEnabledProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<ItemsControl, bool> options;
		options.DefaultValue = false;
		options.Flags = DependencyPropertyFlags::None;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 300;
		options.Design.Order = 60;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence =
			DependencyPropertyPersistence::Metadata;
		)
		options.Changed = [](
			ItemsControl& target, const bool&, const bool& enabled)
		{
			if (!enabled) target.ResetTextSearch();
		};
		return DependencyPropertyRegistry::RegisterStatic<ItemsControl, bool>(
			DependencyPropertyRegistrationLiteral(L"IsTextSearchEnabled"),
			std::move(options));
	}();
	return *registration;
}

const DependencyProperty& ItemsControl::IsTextSearchCaseSensitiveProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<ItemsControl, bool> options;
		options.DefaultValue = false;
		options.Flags = DependencyPropertyFlags::None;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 300;
		options.Design.Order = 70;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence =
			DependencyPropertyPersistence::Metadata;
		)
		options.Changed = [](
			ItemsControl& target, const bool&, const bool&)
		{
			target.ResetTextSearch();
		};
		return DependencyPropertyRegistry::RegisterStatic<ItemsControl, bool>(
			DependencyPropertyRegistrationLiteral(
				L"IsTextSearchCaseSensitive"),
			std::move(options));
	}();
	return *registration;
}

bool ItemsControl::GetIsTextSearchEnabled() const
{
	return GetDependencyPropertyValue<bool>(
		IsTextSearchEnabledProperty());
}

void ItemsControl::SetIsTextSearchEnabled(bool value)
{
	(void)SetDependencyPropertyValue(
		IsTextSearchEnabledProperty(), value);
}

bool ItemsControl::GetIsTextSearchCaseSensitive() const
{
	return GetDependencyPropertyValue<bool>(
		IsTextSearchCaseSensitiveProperty());
}

void ItemsControl::SetIsTextSearchCaseSensitive(bool value)
{
	(void)SetDependencyPropertyValue(
		IsTextSearchCaseSensitiveProperty(), value);
}

const ItemsPanelTemplate& ItemsControl::EffectiveItemsPanel() const noexcept
{
	return _itemsPanel ? *_itemsPanel.Get() : DefaultItemsPanel();
}

bool ItemsControl::IsVirtualizing() const noexcept
{
	return EffectiveItemsPanel().Kind == ItemsPanelKind::VirtualizingStack;
}

void ItemsControl::InvalidateVirtualGroupHeaderMetadata() noexcept
{
	_virtualGroupHeaderMetadataDirty = true;
}

void ItemsControl::RefreshVirtualGroupHeaderMetadata()
{
	if (!_virtualGroupHeaderMetadataDirty) return;
	std::vector<BindingListGroup> groupDefinitions;
	bool grouping = false;
	if (_groupStyle && _itemsSource)
	{
		if (const auto* grouped = dynamic_cast<const IBindingListGroupView*>(
			_itemsSource.Get()))
		{
			const auto& groups = grouped->Groups();
			grouping = !groups.empty();
			if (grouping) groupDefinitions = groups;
		}
	}

	std::vector<uint8_t> bottomLevels;
	std::vector<size_t> starts;
	if (grouping)
	{
		// Canonicalize once per group revision. StartIndex is the lookup key and
		// Level orders coincident hierarchical headers from outermost to innermost.
		// Two stable radix passes keep this O(sizeof(size_t) * G).
		std::vector<size_t> order(groupDefinitions.size());
		for (size_t index = 0; index < order.size(); ++index)
			order[index] = index;
		std::vector<size_t> scratch(order.size());
		StableRadixSortGroupOrder(
			order, scratch, groupDefinitions,
			[](const BindingListGroup& group) { return group.Level; });
		StableRadixSortGroupOrder(
			order, scratch, groupDefinitions,
			[](const BindingListGroup& group) { return group.StartIndex; });
		std::vector<BindingListGroup> normalized;
		normalized.reserve(groupDefinitions.size());
		for (const size_t index : order)
			normalized.push_back(std::move(groupDefinitions[index]));
		groupDefinitions = std::move(normalized);

		// Match CollectionViewGroup.IsBottomLevel without an O(G) scan for each
		// realized header. While walking backwards, this map holds the earliest
		// later start for every level; integer hashing is constant-time here.
		bottomLevels.assign(groupDefinitions.size(), uint8_t{ 1 });
		std::unordered_map<size_t, size_t> nextStartByLevel;
		// Group depth is normally tiny even when G is enormous; do not reserve a
		// million hash buckets for a million sibling groups at the same level.
		nextStartByLevel.reserve((std::min)(
			groupDefinitions.size(), size_t{ 64 }));
		for (size_t index = groupDefinitions.size(); index-- > 0;)
		{
			const auto& group = groupDefinitions[index];
			if (group.Level != (std::numeric_limits<size_t>::max)())
			{
				const auto child = nextStartByLevel.find(group.Level + 1);
				if (child != nextStartByLevel.end()
					&& child->second >= group.StartIndex
					&& child->second - group.StartIndex < group.ItemCount)
					bottomLevels[index] = 0;
			}
			nextStartByLevel[group.Level] = group.StartIndex;
		}

		const size_t itemCount = ItemCount();
		starts.reserve(groupDefinitions.size() * 2);
		for (const auto& group : groupDefinitions)
		{
			if (group.StartIndex >= itemCount) break;
			if (!starts.empty()
				&& starts[starts.size() - 2] == group.StartIndex)
			{
				auto& count = starts.back();
				if (count != (std::numeric_limits<size_t>::max)()) ++count;
				continue;
			}
			starts.push_back(group.StartIndex);
			starts.push_back(1);
		}
	}
	const bool metadataChanged = grouping != _virtualGroupingActive
		|| starts != _virtualGroupHeaderStarts;
	_cachedGroupDefinitions = std::move(groupDefinitions);
	_cachedGroupBottomLevels = std::move(bottomLevels);
	_virtualGroupingActive = grouping;
	if (metadataChanged)
	{
		_virtualGroupHeaderStarts = std::move(starts);
		if (++_virtualGroupHeaderMetadataRevision == 0)
			++_virtualGroupHeaderMetadataRevision;
	}
	_virtualGroupHeaderMetadataDirty = false;
}

void ItemsControl::ConfigureVirtualHost()
{
	auto* host = dynamic_cast<VirtualizingItemsHost*>(_itemsHost);
	if (!host) return;
	RefreshVirtualGroupHeaderMetadata();
	const size_t itemCount = ItemCount();
	host->SetConfiguration(itemCount, GetVirtualizedItemHeight(),
		std::span<const size_t>(_virtualGroupHeaderStarts),
		_virtualGroupHeaderMetadataRevision, _virtualGroupingActive
			? VirtualizedGroupHeaderEstimate : 0.0f,
		GetVirtualizedItemExtentOverrides(),
		GetVirtualizedItemExtentOverridesRevision());
	// The virtual host normally derives horizontal extent from the handful of
	// realized rows. DataGrid column virtualization intentionally omits offscreen
	// cell visuals, so publish the logical full-column width through the host.
	host->SetLogicalExtentWidth(GetVirtualizedHorizontalExtent());
	if (auto* scroll = ItemsScrollOwner())
		scroll->SetLogicalScrollContent(host);
	if (!_itemsSource)
		host->SynchronizeAuthoredItems(_authoredItems);
}

bool ItemsControl::TryUpdateVirtualizedItemExtentOverride(
	size_t itemIndex, double extent)
{
	auto* host = dynamic_cast<VirtualizingItemsHost*>(_itemsHost);
	return host && host->UpdatePersistentItemExtent(
		itemIndex, extent, GetVirtualizedItemExtentOverridesRevision());
}

bool ItemsControl::IsVirtualizedItemInViewport(size_t itemIndex) const noexcept
{
	if (!IsVirtualizing()) return true;
	const auto* host = dynamic_cast<const VirtualizingItemsHost*>(_itemsHost);
	auto* scroll = ItemsScrollOwner();
	if (!host || !scroll || itemIndex >= ItemCount()) return false;
	const auto size = scroll->GetActualSizeDip();
	const double viewport = std::isfinite(size.height)
		? (std::max)(1.0, static_cast<double>(size.height)) : 1.0;
	const double offset = std::isfinite(scroll->VerticalOffset)
		? (std::max)(0.0, scroll->VerticalOffset) : 0.0;
	const double top = host->ItemTop(itemIndex);
	const double bottom = SaturatingDipAdd(top, host->ItemExtent(itemIndex));
	return bottom > offset + 0.0001 && top < offset + viewport - 0.0001;
}

size_t ItemsControl::VirtualOffsetMetadataEntryCount() const noexcept
{
	const auto* host = dynamic_cast<const VirtualizingItemsHost*>(_itemsHost);
	return host ? host->GroupHeaderMetadataCount() : 0;
}

size_t ItemsControl::VirtualOffsetConfigurationRevision() const noexcept
{
	const auto* host = dynamic_cast<const VirtualizingItemsHost*>(_itemsHost);
	return host ? host->ConfigurationRevision() : 0;
}

size_t ItemsControl::VirtualItemExtentOverrideCount() const noexcept
{
	const auto* host = dynamic_cast<const VirtualizingItemsHost*>(_itemsHost);
	return host ? host->PersistentItemExtentOverrideCount() : 0;
}

std::unique_ptr<Panel> ItemsControl::CreateItemsHost() const
{
	const auto& definition = EffectiveItemsPanel();
	std::unique_ptr<Panel> result;
	if (definition.Kind == ItemsPanelKind::Wrap)
	{
		auto panel = std::make_unique<WrapPanel>();
		(void)cui::framework::DependencyPropertyAccess::SetValue(
			*panel, WrapPanel::OrientationProperty(),
			BindingValue(definition.Orientation),
			DependencyPropertyValueSource::Template);
		(void)cui::framework::DependencyPropertyAccess::SetValue(
			*panel, WrapPanel::ItemWidthProperty(),
			BindingValue(definition.ItemWidth),
			DependencyPropertyValueSource::Template);
		(void)cui::framework::DependencyPropertyAccess::SetValue(
			*panel, WrapPanel::ItemHeightProperty(),
			BindingValue(definition.ItemHeight),
			DependencyPropertyValueSource::Template);
		result = std::move(panel);
	}
	else if (definition.Kind == ItemsPanelKind::VirtualizingStack)
	{
		result = std::make_unique<VirtualizingItemsHost>();
	}
	else
	{
		auto panel = std::make_unique<StackPanel>();
		(void)cui::framework::DependencyPropertyAccess::SetValue(
			*panel, StackPanel::OrientationProperty(),
			BindingValue(definition.Orientation),
			DependencyPropertyValueSource::Template);
		result = std::move(panel);
	}
	(void)cui::framework::DependencyPropertyAccess::SetValue(
		*result, Control::VerticalAlignmentProperty(),
		BindingValue(VerticalAlignment::Top),
		DependencyPropertyValueSource::Template);
	return result;
}

std::unique_ptr<Panel> ItemsControl::TakeItemsHost()
{
	if (!_itemsHost) return {};
	auto* requestedHost = _itemsHost;
	const ControlWeakReference hostLifetime(requestedHost);
	if (_templateItemsPresenter
		&& _templateItemsPresenter->GetItemsHost() == _itemsHost)
	{
		auto* presenter = _templateItemsPresenter;
		const ControlWeakReference presenterLifetime(presenter);
		std::unique_ptr<Panel> detached;
		try
		{
			detached = presenter->DetachItemsHost();
		}
		catch (...)
		{
			auto* liveHost = dynamic_cast<Panel*>(hostLifetime.Get());
			auto* livePresenter = dynamic_cast<ItemsPresenter*>(
				presenterLifetime.Get());
			_itemsHost = liveHost && livePresenter
				&& livePresenter->GetItemsHost() == liveHost
				&& liveHost->GetVisualParent() == livePresenter
				&& livePresenter->IndexOfVisualChild(liveHost) >= 0
				? liveHost : nullptr;
			if (!livePresenter && _templateItemsPresenter == presenter)
				_templateItemsPresenter = nullptr;
			throw;
		}
		auto* liveHost = dynamic_cast<Panel*>(hostLifetime.Get());
		auto* livePresenter = dynamic_cast<ItemsPresenter*>(
			presenterLifetime.Get());
		if (detached)
		{
			if (detached.get() != liveHost)
				throw std::logic_error(
					"ItemsPresenter returned an unexpected ItemsHost owner");
			_itemsHost = detached.get();
			return detached;
		}
		if (liveHost && livePresenter
			&& livePresenter->GetItemsHost() == liveHost
			&& liveHost->GetVisualParent() == livePresenter
			&& livePresenter->IndexOfVisualChild(liveHost) >= 0)
		{
			_itemsHost = liveHost;
			return {};
		}
		// A committed callback may transfer or destroy the host and still make
		// DetachItemsHost return no owner. Never retain the old raw slot.
		_itemsHost = nullptr;
		if (!livePresenter && _templateItemsPresenter == presenter)
			_templateItemsPresenter = nullptr;
		return {};
	}
	if (_detachedItemsHost)
	{
		auto detached = std::move(_detachedItemsHost);
		_itemsHost = detached.get();
		return detached;
	}
	if (requestedHost->GetVisualParent() != this
		|| IndexOfVisualChild(requestedHost) < 0)
	{
		_itemsHost = nullptr;
		return {};
	}
	std::unique_ptr<Control> detached;
	try
	{
		DirectVisualMutationFrame mutation(*this, nullptr);
		detached = DetachVisualChild(requestedHost);
	}
	catch (...)
	{
		auto* liveHost = dynamic_cast<Panel*>(hostLifetime.Get());
		_itemsHost = liveHost && liveHost->GetVisualParent() == this
			&& IndexOfVisualChild(liveHost) >= 0 ? liveHost : nullptr;
		throw;
	}
	auto* liveHost = dynamic_cast<Panel*>(hostLifetime.Get());
	if (liveHost && liveHost->GetVisualParent() == this
		&& IndexOfVisualChild(liveHost) >= 0)
	{
		_itemsHost = liveHost;
		return {};
	}
	auto result = std::unique_ptr<Panel>(
		static_cast<Panel*>(detached.release()));
	_itemsHost = result ? result.get() : nullptr;
	return result;
}

void ItemsControl::PlaceItemsHost(std::unique_ptr<Panel> host)
{
	if (!host) throw std::invalid_argument("ItemsControl ItemsHost is null");
	auto* raw = host.get();
	const ControlWeakReference lifetime(raw);
	_itemsHost = raw;
	try
	{
		if (_templateItemsPresenter)
		{
			if (!cui::framework::TreeAccess::
				SetLogicalParentPreservingOwnership(
					host, nullptr))
				throw std::logic_error(
					"ItemsHost ownership changed during logical-parent publication");
			if (host && !host->GetTemplatedParent()
				&& !cui::framework::TreeAccess::
					SetTemplatedParentPreservingOwnership(
						host, this))
				throw std::logic_error(
					"ItemsHost ownership changed during template-parent publication");
			if (!host)
				throw std::logic_error(
					"ItemsHost ownership was transferred before presenter attachment");
			_templateItemsPresenter->SetItemsHost(std::move(host));
			auto* live = dynamic_cast<Panel*>(lifetime.Get());
			if (!live
				|| _templateItemsPresenter->GetItemsHost() != live
				|| live->GetVisualParent() != _templateItemsPresenter)
				throw std::logic_error(
					"ItemsPresenter did not retain the ItemsHost");
		}
		else if (_controlTemplateRoot)
		{
			if (!host->GetTemplatedParent()
				&& !cui::framework::TreeAccess::
					SetTemplatedParentPreservingOwnership(
						host, this))
				throw std::logic_error(
					"ItemsHost ownership changed during template-parent publication");
			if (!host || !cui::framework::TreeAccess::
				SetLogicalParentPreservingOwnership(
					host, nullptr))
				throw std::logic_error(
					"ItemsHost ownership changed during logical-parent publication");
			_detachedItemsHost = std::move(host);
		}
		else
		{
			if (!host->GetTemplatedParent()
				&& !cui::framework::TreeAccess::
					SetTemplatedParentPreservingOwnership(
						host, this))
				throw std::logic_error(
					"ItemsHost ownership changed during template-parent publication");
			try
			{
				DirectVisualMutationFrame mutation(*this, raw);
				(void)cui::framework::TreeAccess::
					InsertOwnedVisualChildPreserving(
						*this, VisualChildCount(),
						host, nullptr);
			}
			catch (...)
			{
				throw;
			}
		}
	}
	catch (...)
	{
		auto* live = dynamic_cast<Panel*>(lifetime.Get());
		if (live && host.get() == live)
			_detachedItemsHost = std::move(host);
		const bool presenterOwns = live && _templateItemsPresenter
			&& _templateItemsPresenter->GetItemsHost() == live
			&& live->GetVisualParent() == _templateItemsPresenter;
		const bool detachedOwner = live
			&& _detachedItemsHost.get() == live;
		const bool directOwner = live
			&& live->GetVisualParent() == this
			&& IndexOfVisualChild(live) >= 0;
		_itemsHost = presenterOwns || detachedOwner || directOwner
			? live : nullptr;
		throw;
	}
	auto* live = dynamic_cast<Panel*>(lifetime.Get());
	const bool presenterOwns = live && _templateItemsPresenter
		&& _templateItemsPresenter->GetItemsHost() == live
		&& live->GetVisualParent() == _templateItemsPresenter
		&& _templateItemsPresenter->IndexOfVisualChild(live) >= 0;
	const bool detachedOwner = live
		&& _detachedItemsHost.get() == live;
	const bool directOwner = live
		&& live->GetVisualParent() == this
		&& IndexOfVisualChild(live) >= 0;
	if (!presenterOwns && !detachedOwner && !directOwner)
	{
		_itemsHost = nullptr;
		throw std::logic_error(
			"ItemsControl ItemsHost placement did not commit");
	}
	_itemsHost = live;
	RefreshItemsScrollOwner();
}

void ItemsControl::ReplaceItemsHostCore(std::unique_ptr<Panel> host)
{
	if (!host) throw std::invalid_argument("ItemsControl ItemsHost is null");
	if (_controlTemplateRoot || _templateItemsPresenter)
		throw std::logic_error("cannot replace a templated ItemsHost");
	if ((_itemsHost && !_itemsHost->GetVisualChildrenView().empty())
		|| _generator.RealizedCount() != 0)
		throw std::logic_error("cannot replace a non-empty ItemsHost");

	auto previous = TakeItemsHost();
	try
	{
		PlaceItemsHost(std::move(host));
	}
	catch (...)
	{
		if (previous) PlaceItemsHost(std::move(previous));
		throw;
	}
	RequestLayout();
}

void ItemsControl::RefreshItemsScrollOwner()
{
	_scrollChanged.Disconnect();
	_itemsScrollOwner = nullptr;
	if (_templateItemsPresenter)
	{
		for (auto* current = _templateItemsPresenter->GetVisualParent();
			current && current != this;
			current = current->GetVisualParent())
		{
			if (auto* scroll = dynamic_cast<ScrollViewer*>(current))
			{
				_itemsScrollOwner = scroll;
				break;
			}
		}
	}
	if (_itemsScrollOwner)
	{
		if (IsVirtualizing())
			_itemsScrollOwner->SetLogicalScrollContent(_itemsHost);
		_scrollChanged = _itemsScrollOwner->OnScrollChanged.Subscribe(
			[this](Control*, ScrollChangedEventArgs&)
			{
				if (!IsVirtualizing() || _applyingCollectionChange) return;
				// A native wheel/thumb burst can publish many offsets before the
				// retained scene presents one frame.  Presented controls consume only
				// the newest range from PreparePresentation; detached controls keep
				// synchronous realization for deterministic programmatic use/tests.
				if (auto* window = GetPresentationWindow();
					window && _itemsScrollOwner
					&& _itemsScrollOwner->_lastScrollChangeWasInteractive)
				{
					_virtualRealizationPending = true;
					// Realize before the next frame snapshots the retained scene,
					// not from the scene traversal itself, so the entering rows are
					// presented in the same frame as the new offset.
					window->RequestPresentationPreparation(*this);
					return;
				}
				(void)RealizeVirtualViewport(true);
			});
	}
}

void ItemsControl::ClearPendingTemplateItemsPresenter() noexcept
{
	_pendingTemplateItemsPresenter.Reset();
}

bool ItemsControl::CommitPendingTemplateItemsPresenter()
{
	const auto candidateReference = _pendingTemplateItemsPresenter;
	auto* candidate = dynamic_cast<ItemsPresenter*>(
		candidateReference.Get());
	if (!candidate || candidate->GetTemplatedParent() != this)
	{
		ClearPendingTemplateItemsPresenter();
		return false;
	}
	auto* root = _controlTemplateRoot;
	if (!root || !VisualSubtreeContains(root, candidate))
		return false;
	if (_templateItemsPresenter)
	{
		const bool alreadyCommitted =
			_templateItemsPresenter == candidate
			&& candidate->GetItemsHost() == _itemsHost
			&& _itemsHost
			&& _itemsHost->GetVisualParent() == candidate
			&& candidate->IndexOfVisualChild(_itemsHost) >= 0;
		if (alreadyCommitted)
			ClearPendingTemplateItemsPresenter();
		return alreadyCommitted;
	}

	const ControlWeakReference rootLifetime(root);
	auto host = TakeItemsHost();
	if (!host)
		throw std::logic_error(
			"ItemsControl lost its ItemsHost while committing ItemsPresenter");
	const ControlWeakReference hostLifetime(host.get());
	candidate = dynamic_cast<ItemsPresenter*>(candidateReference.Get());
	root = rootLifetime.Get();
	if (!candidate || !root || _controlTemplateRoot != root
		|| root->GetVisualParent() != this
		|| IndexOfVisualChild(root) < 0
		|| candidate->GetTemplatedParent() != this
		|| !VisualSubtreeContains(root, candidate))
	{
		ClearPendingTemplateItemsPresenter();
		PlaceItemsHost(std::move(host));
		return false;
	}

	_templateItemsPresenter = candidate;
	try
	{
		_itemsPresenterParentChanged =
			cui::framework::TreeAccess::SubscribeVisualParentChanged(
			*candidate,
			[this](Control* sender, Control*, Control*)
			{
				auto* presenter =
					dynamic_cast<ItemsPresenter*>(sender);
				if (presenter
					&& _templateItemsPresenter == presenter
					&& _controlTemplateRoot
					&& !VisualSubtreeContains(
						_controlTemplateRoot, presenter))
				{
					std::unique_ptr<Panel> recoveredHost;
					auto* presenterHost = presenter->GetItemsHost();
					if (presenterHost
						&& presenterHost == _itemsHost
						&& presenterHost->GetVisualParent()
						== presenter
						&& presenter->IndexOfVisualChild(
							presenterHost) >= 0)
					{
						try
						{
							recoveredHost =
								presenter->DetachItemsHost();
						}
						catch (...) {}
					}
					// The presenter is no longer reachable from the active
					// template root. Clear the template-owner identity while
					// its parent-change transaction still observes any further
					// ownership transfer.
					(void)ClearTemplateOwnerSubtree(
						presenter, this);
					_itemsPresenterParentChanged.Disconnect();
					_templateItemsPresenter = nullptr;
					_itemsHost = nullptr;
					if (recoveredHost)
					{
						try
						{
							PlaceItemsHost(
								std::move(recoveredHost));
						}
						catch (...) {}
					}
				}
				RefreshItemsScrollOwner();
				if (IsVirtualizing())
					(void)RealizeVirtualViewport();
			});
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		_templateItemsPresenter = nullptr;
		_itemsHost = nullptr;
		ClearPendingTemplateItemsPresenter();
		try { PlaceItemsHost(std::move(host)); }
		catch (...) {}
		std::rethrow_exception(originalError);
	}

	auto finalCommitted = [&]() noexcept
	{
		auto* liveRoot = rootLifetime.Get();
		auto* livePresenter = dynamic_cast<ItemsPresenter*>(
			candidateReference.Get());
		auto* liveHost = dynamic_cast<Panel*>(
			hostLifetime.Get());
		return liveRoot && _controlTemplateRoot == liveRoot
			&& liveRoot->GetVisualParent() == this
			&& IndexOfVisualChild(liveRoot) >= 0
			&& livePresenter
			&& _templateItemsPresenter == livePresenter
			&& livePresenter->GetTemplatedParent() == this
			&& VisualSubtreeContains(liveRoot, livePresenter)
			&& liveHost && _itemsHost == liveHost
			&& livePresenter->GetItemsHost() == liveHost
			&& liveHost->GetVisualParent() == livePresenter
			&& livePresenter->IndexOfVisualChild(liveHost) >= 0;
	};
	auto rollback = [&]() noexcept
	{
		std::unique_ptr<Panel> recoveredHost;
		auto* livePresenter = dynamic_cast<ItemsPresenter*>(
			candidateReference.Get());
		auto* liveHost = dynamic_cast<Panel*>(
			hostLifetime.Get());
		if (livePresenter && liveHost
			&& livePresenter->GetItemsHost() == liveHost
			&& liveHost->GetVisualParent() == livePresenter
			&& livePresenter->IndexOfVisualChild(liveHost) >= 0)
		{
			try { recoveredHost = livePresenter->DetachItemsHost(); }
			catch (...) {}
		}
		auto* liveRoot = rootLifetime.Get();
		livePresenter = dynamic_cast<ItemsPresenter*>(
			candidateReference.Get());
		if (livePresenter
			&& livePresenter->GetTemplatedParent() == this
			&& (!liveRoot
				|| !VisualSubtreeContains(
					liveRoot, livePresenter)))
			(void)ClearTemplateOwnerSubtree(
				livePresenter, this);
		if (!recoveredHost && liveHost
			&& _detachedItemsHost.get() == liveHost)
			recoveredHost = std::move(_detachedItemsHost);
		_itemsPresenterParentChanged.Disconnect();
		_templateItemsPresenter = nullptr;
		_itemsHost = nullptr;
		ClearPendingTemplateItemsPresenter();
		if (recoveredHost)
		{
			try { PlaceItemsHost(std::move(recoveredHost)); }
			catch (...) {}
		}
	};

	try
	{
		PlaceItemsHost(std::move(host));
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		if (finalCommitted())
		{
			ClearPendingTemplateItemsPresenter();
			RefreshItemsScrollOwner();
			std::rethrow_exception(originalError);
		}
		rollback();
		std::rethrow_exception(originalError);
	}
	if (!finalCommitted())
	{
		rollback();
		throw std::logic_error(
			"ItemsControl ItemsPresenter commitment did not retain the active root and ItemsHost");
	}
	ClearPendingTemplateItemsPresenter();
	RefreshItemsScrollOwner();
	return true;
}

bool ItemsControl::RegisterTemplateItemsPresenter(
	ItemsPresenter* presenter)
{
	if (!presenter || presenter->GetTemplatedParent() != this)
		return false;
	if (_templateItemsPresenter == presenter) return true;
	if (_templateItemsPresenter
		&& _templateItemsPresenter != presenter)
		return false;
	_pendingTemplateItemsPresenter = presenter;
	if (_controlTemplateRoot)
	{
		if (!VisualSubtreeContains(_controlTemplateRoot, presenter))
		{
			ClearPendingTemplateItemsPresenter();
			return false;
		}
		return CommitPendingTemplateItemsPresenter();
	}
	RequestLayout();
	return true;
}

Control* ItemsControl::SetControlTemplateRoot(
	std::unique_ptr<Control> value)
{
	if (value && value.get() == _controlTemplateRoot)
	{
		(void)value.release();
		return _controlTemplateRoot;
	}
	if (!value)
	{
		(void)DetachVisualChildTemplateRoot();
		return nullptr;
	}
	if (_controlTemplateRoot)
		throw std::logic_error("ItemsControl already owns a ControlTemplate root");
	auto* candidateRoot = value.get();
	if (auto* pending = dynamic_cast<ItemsPresenter*>(
		_pendingTemplateItemsPresenter.Get()))
	{
		if (pending->GetTemplatedParent() != this)
			ClearPendingTemplateItemsPresenter();
		else if (!VisualSubtreeContains(candidateRoot, pending)
			&& pending->GetVisualParent())
		{
			ClearPendingTemplateItemsPresenter();
			throw std::logic_error(
				"ItemsControl pending ItemsPresenter does not belong to the candidate template root");
		}
	}
	else if (_pendingTemplateItemsPresenter.HasValue())
		ClearPendingTemplateItemsPresenter();
	if (!_templateItemsPresenter)
	{
		auto host = TakeItemsHost();
		if (!host) throw std::logic_error("ItemsControl lost its ItemsHost");
		_detachedItemsHost = std::move(host);
	}
	const ControlWeakReference candidateLifetime(candidateRoot);
	const ControlWeakReference presenterLifetime(
		_templateItemsPresenter);
	_controlTemplateRoot = candidateRoot;
	try
	{
		DirectVisualMutationFrame mutation(*this, candidateRoot);
		(void)cui::framework::TreeAccess::
			InsertOwnedVisualChildPreserving(
				*this, VisualChildCount(), value, nullptr);
		auto* live = candidateLifetime.Get();
		if (!live || live->GetVisualParent() != this
			|| IndexOfVisualChild(live) < 0)
			throw std::logic_error(
				"ItemsControl template root attachment did not commit");
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		auto* live = candidateLifetime.Get();
		if (live && (live->GetVisualParent() == this
			|| IndexOfVisualChild(live) >= 0))
		{
			// The collection owns a post-commit failure. Keep it published so
			// the caller's AbortTemplateApplication can detach it normally.
			_controlTemplateRoot = live;
			MarkControlTemplateRootAttached();
		}
		else
		{
			std::unique_ptr<Panel> host;
			auto* presenter = dynamic_cast<ItemsPresenter*>(
				presenterLifetime.Get());
			if (presenter && _templateItemsPresenter == presenter)
			{
				auto* presenterHost = presenter->GetItemsHost();
				if (presenterHost && presenterHost == _itemsHost
					&& presenterHost->GetVisualParent() == presenter)
				{
					try { host = presenter->DetachItemsHost(); }
					catch (...) {}
				}
			}
			if (!host && _detachedItemsHost)
				host = std::move(_detachedItemsHost);
			_itemsPresenterParentChanged.Disconnect();
			_templateItemsPresenter = nullptr;
			_itemsHost = nullptr;
			ClearPendingTemplateItemsPresenter();
			if (_controlTemplateRoot == candidateRoot)
				_controlTemplateRoot = nullptr;
			if (value)
				(void)ClearTemplateOwnerSubtreePreservingOwnership(
					value, this);
			else
				(void)ClearTemplateOwnerSubtree(live, this);
			try { ClearDeclarativeTemplateScope(); }
			catch (...) {}
			if (host)
			{
				try { PlaceItemsHost(std::move(host)); }
				catch (...) {}
			}
			MarkControlTemplateRootDetached();
			try { OnControlTemplatePresentationChanged(); }
			catch (...) {}
		}
		std::rethrow_exception(originalError);
	}
	auto* liveRoot = candidateLifetime.Get();
	if (!liveRoot || liveRoot->GetVisualParent() != this
		|| IndexOfVisualChild(liveRoot) < 0)
		throw std::logic_error(
			"ItemsControl template root ownership is invalid");
	_controlTemplateRoot = liveRoot;
	MarkControlTemplateRootAttached();
	try
	{
		if (auto* pending = dynamic_cast<ItemsPresenter*>(
			_pendingTemplateItemsPresenter.Get()))
		{
			if (pending->GetTemplatedParent() != this
				|| (pending->GetVisualParent()
					&& !VisualSubtreeContains(liveRoot, pending)))
			{
				ClearPendingTemplateItemsPresenter();
				throw std::logic_error(
					"ItemsControl pending ItemsPresenter escaped the active template root");
			}
			(void)CommitPendingTemplateItemsPresenter();
		}
		if (_templateItemsPresenter
			&& !VisualSubtreeContains(
				liveRoot, _templateItemsPresenter))
			throw std::logic_error(
				"ItemsControl active ItemsPresenter is outside the template root");
		OnControlTemplatePresentationChanged();
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		try { (void)DetachVisualChildTemplateRoot(); }
		catch (...) {}
		std::rethrow_exception(originalError);
	}
	RefreshItemsScrollOwner();
	RequestLayout();
	InvalidateVisual();
	return _controlTemplateRoot;
}

std::unique_ptr<Control> ItemsControl::DetachVisualChildTemplateRoot()
{
	if (!_controlTemplateRoot)
	{
		auto host = TakeItemsHost();
		_itemsPresenterParentChanged.Disconnect();
		_templateItemsPresenter = nullptr;
		ClearPendingTemplateItemsPresenter();
		ClearDeclarativeTemplateScope();
		if (host) PlaceItemsHost(std::move(host));
		MarkControlTemplateRootDetached();
		OnControlTemplatePresentationChanged();
		RequestLayout();
		InvalidateVisual();
		return {};
	}
	auto* previous = _controlTemplateRoot;
	const ControlWeakReference previousLifetime(previous);
	const ControlWeakReference presenterLifetime(
		_templateItemsPresenter);
	std::unique_ptr<Control> root;
	std::exception_ptr notificationError;
	bool visualOwnershipCommit = false;
	try
	{
		DirectVisualMutationFrame mutation(*this, nullptr);
		root = cui::framework::TreeAccess::DetachVisualChild(
			*this, previous, &visualOwnershipCommit,
			&notificationError);
	}
	catch (...)
	{
		auto* live = previousLifetime.Get();
		if (live && live->GetVisualParent() == this
			&& IndexOfVisualChild(live) >= 0)
		{
			_controlTemplateRoot = live;
			MarkControlTemplateRootAttached();
		}
		throw;
	}

	auto* livePrevious = previousLifetime.Get();
	if (livePrevious && livePrevious->GetVisualParent() == this
		&& IndexOfVisualChild(livePrevious) >= 0)
	{
		_controlTemplateRoot = livePrevious;
		MarkControlTemplateRootAttached();
		if (notificationError)
			std::rethrow_exception(notificationError);
		throw std::logic_error(
			"ItemsControl template root detach did not commit");
	}

	std::exception_ptr cleanupError;
	std::unique_ptr<Panel> host;
	auto* presenter = dynamic_cast<ItemsPresenter*>(
		presenterLifetime.Get());
	if (presenter && _templateItemsPresenter == presenter)
	{
		auto* presenterHost = presenter->GetItemsHost();
		if (presenterHost && presenterHost == _itemsHost
			&& presenterHost->GetVisualParent() == presenter)
		{
			try { host = presenter->DetachItemsHost(); }
			catch (...)
			{
				cleanupError = std::current_exception();
			}
		}
	}
	if (!host && _detachedItemsHost)
		host = std::move(_detachedItemsHost);
	_itemsPresenterParentChanged.Disconnect();
	_templateItemsPresenter = nullptr;
	ClearPendingTemplateItemsPresenter();
	_itemsHost = nullptr;
	_controlTemplateRoot = nullptr;
	auto ownerError = root
		? ClearTemplateOwnerSubtreePreservingOwnership(root, this)
		: ClearTemplateOwnerSubtree(livePrevious, this);
	if (!cleanupError) cleanupError = ownerError;
	try { ClearDeclarativeTemplateScope(); }
	catch (...)
	{
		if (!cleanupError) cleanupError = std::current_exception();
	}
	if (host)
	{
		try { PlaceItemsHost(std::move(host)); }
		catch (...)
		{
			if (!cleanupError) cleanupError = std::current_exception();
		}
	}
	MarkControlTemplateRootDetached();
	try { OnControlTemplatePresentationChanged(); }
	catch (...)
	{
		if (!cleanupError) cleanupError = std::current_exception();
	}
	RequestLayout();
	InvalidateVisual();
	if (!cleanupError) cleanupError = notificationError;
	if (cleanupError)
		std::rethrow_exception(cleanupError);
	return root;
}

void ItemsControl::SetItemsPanel(ItemsPanelTemplateReference value)
{
	if (_migratingAuthoredItems)
		throw std::logic_error(
			"ItemsControl does not support reentrant ItemsHost migration");
	if (_itemsPanel == value) return;
	const auto& definition = value ? *value.Get() : DefaultItemsPanel();
	if (!IsValidItemsPanel(definition))
	{
		_lastTemplateError = L"ItemsPanelTemplate 配置无效；VirtualizingStackPanel 需要正数 ItemHeight 且只支持 Vertical。";
		return;
	}
	(void)ReplaceItemsHost(std::move(value));
}

bool ItemsControl::ReplaceItemsHost(ItemsPanelTemplateReference value)
{
	const auto previousPanel = _itemsPanel;
	_itemsPanel = std::move(value);
	std::unique_ptr<Panel> replacement;
	try
	{
		replacement = CreateItemsHost();
	}
	catch (...)
	{
		_itemsPanel = previousPanel;
		_lastTemplateError = L"ItemsPanelTemplate 无法创建 ItemsHost。";
		return false;
	}

	if (!_authoredItems.empty())
	{
		std::vector<ControlWeakReference> authoredItems;
		std::vector<Control*> restoredItems;
		authoredItems.reserve(_authoredItems.size());
		restoredItems.reserve(_authoredItems.size());
		for (auto* item : _authoredItems)
			authoredItems.emplace_back(item);

		_migratingAuthoredItems = true;
		struct AuthoredMigrationGuard final
		{
			bool& Value;
			~AuthoredMigrationGuard() { Value = false; }
		} migrationGuard{ _migratingAuthoredItems };
		// Raw authored slots are unpublished for the complete callback-bearing
		// transaction. They are reconstructed only from a verified final host.
		_authoredItems.clear();
		auto oldGenerator = std::move(_generator);
		std::unique_ptr<Panel> oldHost;
		std::unique_ptr<Control> inFlight;
		try
		{
			oldHost = TakeItemsHost();
			if (!oldHost)
				throw std::logic_error(
					"ItemsControl authored ItemsHost ownership is invalid");
			const ControlWeakReference oldHostLifetime(oldHost.get());
			if (!cui::framework::TreeAccess::
				InvokePreservingVisualOwnership(
					oldHost,
					[&](Control&)
					{
						PlaceItemsHost(std::move(replacement));
					}))
				throw std::logic_error(
					"ItemsControl previous ItemsHost ownership escaped during migration");
			auto* replacementHost = _itemsHost;
			const ControlWeakReference replacementHostLifetime(
				replacementHost);
			if (!replacementHost)
				throw std::logic_error(
					"ItemsControl replacement ItemsHost did not commit");

			for (const auto& itemReference : authoredItems)
			{
				std::exception_ptr notificationError;
				const bool retainedOldHost =
					cui::framework::TreeAccess::
					InvokePreservingVisualOwnership(
						oldHost,
						[&](Control& previousRoot)
						{
							auto* previousHost =
								dynamic_cast<Panel*>(&previousRoot);
							auto* nextHost = dynamic_cast<Panel*>(
								replacementHostLifetime.Get());
							auto* liveItem = itemReference.Get();
							if (!previousHost || !nextHost
								|| _itemsHost != nextHost || !liveItem
								|| liveItem->GetVisualParent()
								!= previousHost
								|| previousHost->IndexOfVisualChild(
									liveItem) < 0)
								throw std::logic_error(
									"ItemsControl authored item ownership changed during migration");
							bool ownershipCommit = false;
							inFlight =
								cui::framework::TreeAccess::DetachVisualChild(
									*previousHost, liveItem,
									&ownershipCommit,
									&notificationError);
							if (!inFlight
								|| inFlight.get() != itemReference.Get())
								throw std::logic_error(
									"ItemsControl authored item detach did not commit");
							(void)cui::framework::TreeAccess::
								InsertOwnedVisualChildPreserving(
									*nextHost,
									nextHost->VisualChildCount(),
									inFlight, this);
							liveItem = itemReference.Get();
							if (!liveItem
								|| liveItem->GetVisualParent() != nextHost
								|| nextHost->IndexOfVisualChild(
									liveItem) < 0
								|| liveItem->GetLogicalParent() != this)
								throw std::logic_error(
									"ItemsControl authored item attachment did not commit");
							if (notificationError)
								std::rethrow_exception(notificationError);
						});
				if (!retainedOldHost)
					throw std::logic_error(
						"ItemsControl previous ItemsHost was transferred during migration");
			}

			auto* liveReplacement = dynamic_cast<Panel*>(
				replacementHostLifetime.Get());
			auto* liveOldHost = dynamic_cast<Panel*>(
				oldHostLifetime.Get());
			if (!liveReplacement || _itemsHost != liveReplacement
				|| !liveOldHost || oldHost.get() != liveOldHost
				|| liveReplacement->VisualChildCount()
				!= static_cast<int>(authoredItems.size()))
				throw std::logic_error(
					"ItemsControl authored migration final host is invalid");
			const auto children =
				liveReplacement->GetVisualChildrenView();
			for (size_t index = 0;
				index < authoredItems.size(); ++index)
			{
				auto* liveItem = authoredItems[index].Get();
				if (!liveItem || children[index] != liveItem
					|| liveItem->GetVisualParent() != liveReplacement
					|| liveItem->GetLogicalParent() != this)
					throw std::logic_error(
						"ItemsControl authored migration order did not commit");
				restoredItems.push_back(liveItem);
			}
			_authoredItems = std::move(restoredItems);
			_lastTemplateError.clear();
			RequestLayout();
			InvalidateVisual();
			return true;
		}
		catch (...)
		{
			// Put an interrupted detach back first; never carry an owner token
			// across later callback-bearing rollback operations.
			if (inFlight && oldHost)
			{
				try
				{
					(void)cui::framework::TreeAccess::
						InvokePreservingVisualOwnership(
							oldHost,
							[&](Control& previousRoot)
							{
								(void)cui::framework::TreeAccess::
									InsertOwnedVisualChildPreserving(
										previousRoot,
										previousRoot.VisualChildCount(),
										inFlight, this);
							});
				}
				catch (...) {}
			}
			inFlight.reset();

			for (const auto& itemReference : authoredItems)
			{
				if (!oldHost) break;
				try
				{
					(void)cui::framework::TreeAccess::
						InvokePreservingVisualOwnership(
							oldHost,
							[&](Control& previousRoot)
							{
								auto* previousHost =
									dynamic_cast<Panel*>(&previousRoot);
								auto* liveItem = itemReference.Get();
								if (!previousHost || !liveItem
									|| (liveItem->GetVisualParent()
										== previousHost
										&& previousHost->
										IndexOfVisualChild(liveItem) >= 0))
									return;
								auto* currentHost = _itemsHost;
								if (!currentHost
									|| liveItem->GetVisualParent()
									!= currentHost
									|| currentHost->IndexOfVisualChild(
										liveItem) < 0)
									return;
								bool ownershipCommit = false;
								std::exception_ptr ignoredNotificationError;
								auto owner =
									cui::framework::TreeAccess::
									DetachVisualChild(
										*currentHost, liveItem,
										&ownershipCommit,
										&ignoredNotificationError);
								if (!owner) return;
								try
								{
									(void)cui::framework::TreeAccess::
										InsertOwnedVisualChildPreserving(
											*previousHost,
											previousHost->VisualChildCount(),
											owner, this);
								}
								catch (...) {}
							});
				}
				catch (...) {}
			}
			if (oldHost)
			{
				try
				{
					(void)cui::framework::TreeAccess::
						InvokePreservingVisualOwnership(
							oldHost,
							[&](Control& previousRoot)
							{
								auto* previousHost =
									dynamic_cast<Panel*>(&previousRoot);
								if (!previousHost) return;
								size_t targetIndex = 0;
								for (const auto& itemReference
									: authoredItems)
								{
									auto* liveItem = itemReference.Get();
									if (!liveItem
										|| liveItem->GetVisualParent()
										!= previousHost)
										continue;
									const int currentIndex =
										previousHost->
										IndexOfVisualChild(liveItem);
									if (currentIndex >= 0
										&& currentIndex
										!= static_cast<int>(
											targetIndex))
										(void)previousHost->
											MoveVisualChild(
												currentIndex,
												static_cast<int>(
													targetIndex));
									++targetIndex;
								}
							});
				}
				catch (...) {}
			}

			try
			{
				if (oldHost)
				{
					(void)cui::framework::TreeAccess::
						InvokePreservingVisualOwnership(
							oldHost,
							[&](Control&)
							{
								auto failedHost = TakeItemsHost();
								(void)failedHost;
							});
				}
				else
				{
					auto failedHost = TakeItemsHost();
					(void)failedHost;
				}
			}
			catch (...) {}
			_itemsPanel = previousPanel;
			if (oldHost)
			{
				try { PlaceItemsHost(std::move(oldHost)); }
				catch (...) {}
			}
			_generator = std::move(oldGenerator);
			AdvanceGeneratedItemsRevision();
			restoredItems.clear();
			auto* restoredHost = _itemsHost;
			for (const auto& itemReference : authoredItems)
			{
				auto* liveItem = itemReference.Get();
				if (restoredHost && liveItem
					&& liveItem->GetVisualParent() == restoredHost
					&& restoredHost->IndexOfVisualChild(liveItem) >= 0
					&& liveItem->GetLogicalParent() == this)
					restoredItems.push_back(liveItem);
			}
			_authoredItems = std::move(restoredItems);
			_lastTemplateError = L"ItemsPanelTemplate 无法迁移 authored Items。";
			return false;
		}
	}

	auto oldGenerator = std::move(_generator);
	auto oldHost = TakeItemsHost();
	if (!oldHost)
	{
		_itemsPanel = previousPanel;
		_generator = std::move(oldGenerator);
		AdvanceGeneratedItemsRevision();
		_lastTemplateError = L"ItemsHost 所有权无效。";
		return false;
	}
	PlaceItemsHost(std::move(replacement));
	AdvanceGeneratedItemsRevision();
	if (RebuildGeneratedItems()) return true;
	const auto error = _lastTemplateError;
	auto failedHost = TakeItemsHost();
	_itemsPanel = previousPanel;
	PlaceItemsHost(std::move(oldHost));
	_generator = std::move(oldGenerator);
	AdvanceGeneratedItemsRevision();
	_lastTemplateError = error;
	return false;
}

void ItemsControl::SetItemsSource(BindingListReference value)
{
	const ControlWeakReference ownerLifetime(this);
	if (_itemsSource == value) return;
	if (value && !_authoredItems.empty())
		throw std::logic_error(
			"ItemsControl Items and ItemsSource cannot both be populated");
	if (IsItemsSourceUpdateInProgress())
		throw std::logic_error(
			"ItemsControl does not support reentrant ItemsSource changes");
	BindingListReference candidateSnapshot;
	std::wstring snapshotError;
	auto candidateHost = CreateItemsHost();
	auto derivedState = CaptureItemsSourceTransactionState();
	_itemsSourceReplacementInProgress = true;
	ItemsScopeExit replacementGuard([ownerLifetime]() noexcept
	{
		if (auto* live = dynamic_cast<ItemsControl*>(ownerLifetime.Get()))
			live->_itemsSourceReplacementInProgress = false;
	});
	++_itemsSourceUpdateDepth;
	const auto previous = _itemsSource;
	const auto previousSnapshot = _materializedItemsSourceSnapshot;
	// Keep the old subscriptions alive until the candidate source and its
	// complete generated tree commit. This makes a failed/throwing template
	// rollback allocation-free and preserves the previous live view.
	auto previousItemsSourceChanged = std::move(_itemsSourceChanged);
	auto previousGroupsChanged = std::move(_groupsChanged);
	_itemsSource = std::move(value);
	InvalidateVirtualGroupHeaderMetadata();
	auto restorePrevious = [&]() noexcept
	{
		_itemsSourceChanged.Disconnect();
		_groupsChanged.Disconnect();
		_itemsSource = previous;
		InvalidateVirtualGroupHeaderMetadata();
		_itemsSourceChanged = std::move(previousItemsSourceChanged);
		_groupsChanged = std::move(previousGroupsChanged);
	};
	auto restoreDerivedState = [&]() noexcept
	{
		if (!derivedState) return;
		RestoreItemsSourceTransactionState(*derivedState);
		derivedState.reset();
	};
	try
	{
		if (_itemsSource)
		{
			auto* sourceIdentity = _itemsSource.Get();
			_itemsSourceChanged = _itemsSource.Get()->SubscribeChanged(
				[this, sourceIdentity](const CollectionChangedEventArgs& change)
				{
					if (_itemsSource.Get() == sourceIdentity
						&& !IsItemsSourceUpdateInProgress())
						HandleItemsSourceChange(change);
				});
			if (auto* grouped = dynamic_cast<IBindingListGroupView*>(
				_itemsSource.Get()))
				_groupsChanged = grouped->SubscribeGroupsChanged(
					[this, sourceIdentity]
					{
						if (_itemsSource.Get() == sourceIdentity
							&& !IsItemsSourceUpdateInProgress())
						{
							InvalidateVirtualGroupHeaderMetadata();
							RefreshGroupHeaders();
						}
					});
		}
	}
	catch (...)
	{
		restorePrevious();
		restoreDerivedState();
		--_itemsSourceUpdateDepth;
		throw;
	}
	// A non-standard IBindingList may publish synchronously from SubscribeChanged.
	// Those notifications are suppressed above, so capture only after all
	// subscriptions exist and materialize the candidate's final state. Capturing
	// before and after subscription would perform two complete source scans while
	// providing no additional rollback state: the old host/source remain committed
	// until this final snapshot succeeds.
	try
	{
		if (!TryCaptureItemsSourceSnapshot(
			_itemsSource, candidateSnapshot, snapshotError))
		{
			restorePrevious();
			restoreDerivedState();
			--_itemsSourceUpdateDepth;
			_lastTemplateError = std::move(snapshotError);
			return;
		}
	}
	catch (...)
	{
		restorePrevious();
		restoreDerivedState();
		--_itemsSourceUpdateDepth;
		throw;
	}

	// Materialize against an off-tree host. The currently committed host and
	// generator stay alive until both generation and the derived source-change
	// hook have committed, so a throwing hook cannot strand a new source above
	// the old visual tree (or destroy the tree needed for rollback).
	auto* previousHostRaw = _itemsHost;
	auto previousGenerator = std::move(_generator);
	_generator = ItemContainerGenerator{};
	AdvanceGeneratedItemsRevision();
	_itemsHost = candidateHost.get();
	bool rebuilt = false;
	try
	{
		OnItemsSourceReplacementPreparing(previous, _itemsSource);
		if (!ownerLifetime.Get()) return;
		rebuilt = RebuildGeneratedItems();
	}
	catch (...)
	{
		_itemsHost = previousHostRaw;
		_generator = std::move(previousGenerator);
		AdvanceGeneratedItemsRevision();
		_materializedItemsSourceSnapshot = previousSnapshot;
		restorePrevious();
		restoreDerivedState();
		--_itemsSourceUpdateDepth;
		throw;
	}
	if (!ownerLifetime.Get()) return;
	if (!rebuilt)
	{
		const auto error = _lastTemplateError;
		_itemsHost = previousHostRaw;
		_generator = std::move(previousGenerator);
		AdvanceGeneratedItemsRevision();
		_materializedItemsSourceSnapshot = previousSnapshot;
		restorePrevious();
		restoreDerivedState();
		--_itemsSourceUpdateDepth;
		_lastTemplateError = error;
		return;
	}

	std::unique_ptr<Panel> previousHost;
	try
	{
		_itemsHost = previousHostRaw;
		previousHost = TakeItemsHost();
		if (!previousHost)
			throw std::logic_error("ItemsControl lost its committed ItemsHost");
		PlaceItemsHost(std::move(candidateHost));
	}
	catch (...)
	{
		// A placement failure is rare (normally allocation failure), but the
		// logical source and generator still have the same strong guarantee.
		if (previousHost)
		{
			try
			{
				if (_itemsHost && _itemsHost != previousHost.get())
				{
					auto failedHost = TakeItemsHost();
					(void)failedHost;
				}
				PlaceItemsHost(std::move(previousHost));
			}
			catch (...) {}
		}
		else _itemsHost = previousHostRaw;
		_generator = std::move(previousGenerator);
		AdvanceGeneratedItemsRevision();
		_materializedItemsSourceSnapshot = previousSnapshot;
		restorePrevious();
		restoreDerivedState();
		--_itemsSourceUpdateDepth;
		throw;
	}

	const auto candidate = _itemsSource;
	try
	{
		OnItemsSourceChanged(previous, candidate);
	}
	catch (...)
	{
		const auto failure = std::current_exception();
		try
		{
			auto failedHost = TakeItemsHost();
			(void)failedHost;
			PlaceItemsHost(std::move(previousHost));
		}
		catch (...) {}
		_generator = std::move(previousGenerator);
		AdvanceGeneratedItemsRevision();
		_materializedItemsSourceSnapshot = previousSnapshot;
		restorePrevious();
		// Derived controls may have committed secondary state (for example a
		// current-view subscription) before throwing. Give them the symmetric
		// notification after the base state is restored; preserve the original
		// exception if even this best-effort rollback hook fails.
		try { OnItemsSourceChanged(candidate, previous); }
		catch (...) {}
		restoreDerivedState();
		--_itemsSourceUpdateDepth;
		std::rethrow_exception(failure);
	}
	_materializedItemsSourceSnapshot = std::move(candidateSnapshot);
	ResetTextSearch();
	--_itemsSourceUpdateDepth;
	_itemsSourceReplacementInProgress = false;
	OnItemsSourceTransactionCommitted();
}

void ItemsControl::SetCustomProjectionItemsSource(BindingListReference value)
{
	if (value && !_authoredItems.empty())
		throw std::logic_error(
			"ItemsControl Items and ItemsSource cannot both be populated");
	_itemsSource = std::move(value);
	InvalidateVirtualGroupHeaderMetadata();
	ResetTextSearch();
}

void ItemsControl::HandleItemsSourceChange(
	const CollectionChangedEventArgs& change)
{
	const ControlWeakReference ownerLifetime(this);
	const auto failedSource = _itemsSource;
	PreparedMaterializedSnapshotChange preparedSnapshot;
	std::wstring snapshotError;
	std::unique_ptr<ItemsSourceTransactionState> derivedState;
	bool committed = false;
	++_itemsSourceUpdateDepth;
	auto restoreDerivedState = [&]() noexcept
	{
		if (!derivedState) return;
		RestoreItemsSourceTransactionState(*derivedState);
		derivedState.reset();
	};
	auto rollbackToMaterializedSnapshot = [&]
	{
		_itemsSourceChanged.Disconnect();
		_groupsChanged.Disconnect();
		_itemsSource = _materializedItemsSourceSnapshot;
		InvalidateVirtualGroupHeaderMetadata();
		try { OnItemsSourceChanged(failedSource, _itemsSource); }
		catch (...) {}
	};
	try
	{
		// A live collection has already mutated before publishing this callback.
		// CollectionView updates Groups before Changed and publishes GroupsChanged
		// only after this callback returns. Refresh now so replacement containers
		// are never prepared against the previous group-start cache.
		InvalidateVirtualGroupHeaderMetadata();
		RefreshVirtualGroupHeaderMetadata();
		// Capture inside the guarded region so even token allocation failure pins
		// the control back to its last materialized snapshot.
		derivedState = CaptureItemsSourceTransactionState();
		const bool prepared = TryPrepareMaterializedSnapshotChange(
			_itemsSource, _materializedItemsSourceSnapshot,
			change, preparedSnapshot, snapshotError);
		bool rebuilt = false;
		if (prepared)
		{
			OnItemsSourceCollectionChangePreparing(
				change, _materializedItemsSourceSnapshot);
			if (!ownerLifetime.Get()) return;
			rebuilt = ApplyCollectionChange(change);
			if (!ownerLifetime.Get()) return;
			if (!rebuilt) rebuilt = RebuildGeneratedItems();
			if (!ownerLifetime.Get()) return;
		}
		if (rebuilt)
		{
			if (!preparedSnapshot.Replace || !preparedSnapshot.Replacement)
				throw std::logic_error(
					"ItemsControl prepared snapshot replacement is unavailable");
			_materializedItemsSourceSnapshot =
				std::move(preparedSnapshot.Replacement);
			ResetTextSearch();
			derivedState.reset();
			committed = true;
		}
		else
		{
			const auto error = prepared
				? _lastTemplateError : snapshotError;
			rollbackToMaterializedSnapshot();
			restoreDerivedState();
			_lastTemplateError = error;
		}
		--_itemsSourceUpdateDepth;
	}
	catch (...)
	{
		const auto failure = std::current_exception();
		const auto error = _lastTemplateError;
		rollbackToMaterializedSnapshot();
		// ApplyCollectionChange prepares template visuals before touching the
		// committed tree, but virtual/layout and derived rebuilt callbacks are
		// extensible and may throw after commit. Re-materialize the immutable old
		// snapshot so even that exceptional boundary converges source and visuals.
		try { (void)RebuildGeneratedItems(); }
		catch (...) {}
		restoreDerivedState();
		_lastTemplateError = error;
		--_itemsSourceUpdateDepth;
		std::rethrow_exception(failure);
	}
	if (committed)
	{
		OnItemsSourceCollectionChangeCommitted(change);
		if (!ownerLifetime.Get()) return;
		OnItemsSourceTransactionCommitted();
	}
}

void ItemsControl::SetItemTemplate(ItemTemplateReference value)
{
	if (_itemTemplate == value) return;
	const auto previous = _itemTemplate;
	_itemTemplate = std::move(value);
	if (RebuildGeneratedItems())
	{
		ResetTextSearch();
		return;
	}
	const auto error = _lastTemplateError;
	_itemTemplate = previous;
	_lastTemplateError = error;
}

void ItemsControl::SetGroupStyle(GroupStyleReference value)
{
	if (_groupStyle == value) return;
	const auto previous = _groupStyle;
	_groupStyle = std::move(value);
	InvalidateVirtualGroupHeaderMetadata();
	if (RebuildGeneratedItems()) return;
	const auto error = _lastTemplateError;
	_groupStyle = previous;
	InvalidateVirtualGroupHeaderMetadata();
	(void)RebuildGeneratedItems();
	_lastTemplateError = error;
}

void ItemsControl::SetCompiledDisplayMemberPath(
	CompiledBindingPathView value)
{
	ValidateCompiledMemberPath(value, "DisplayMemberPath");
	if (SameCompiledBindingPath(_compiledDisplayMemberPath, value)
#if CUI_ENABLE_DYNAMIC_XAML
		&& _displayMemberPath.empty()
#endif
		) return;
	_compiledDisplayMemberPath = value;
#if CUI_ENABLE_DYNAMIC_XAML
	_displayMemberPath.clear();
#endif
	ResetTextSearch();
	if (_itemsSource && !_itemTemplate) (void)RebuildGeneratedItems();
}

std::wstring ItemsControl::GetDisplayMemberText(
	const BindingSourceReference& item) const
{
	if (!_compiledDisplayMemberPath.Empty())
		return GetBindingRecordText(item, _compiledDisplayMemberPath);
#if CUI_ENABLE_DYNAMIC_XAML
	return ReadAuthoredDisplayMemberText(item);
#else
	return {};
#endif
}

BindingPathObservation ItemsControl::ObserveDisplayMemberPath(
	const BindingSourceReference& item,
	std::function<void()> changed) const
{
	if (!_compiledDisplayMemberPath.Empty())
		return ObserveBindingPaths(
			item, { _compiledDisplayMemberPath }, std::move(changed));
#if CUI_ENABLE_DYNAMIC_XAML
	return ObserveAuthoredDisplayMemberPath(item, std::move(changed));
#else
	return {};
#endif
}

std::wstring ItemsControl::GetTextSearchItemText(size_t index) const
{
	if (index >= ItemCount()) return {};
	if (!_itemsSource)
	{
		auto* item = GetAuthoredItem(index);
		return item ? item->GetDisplayText() : std::wstring{};
	}

	BindingSourceReference item;
	if (!_itemsSource.Get()->TryGetItem(index, item) || !item) return {};
	auto result = GetDisplayMemberText(item);
	if (!result.empty()) return result;

	// An authored DataTemplate can expose a better semantic projection than a
	// record with no DisplayMemberPath. Use it when that item is already
	// realized, without forcing realization as a side effect of searching.
	auto* realized = GetGeneratedItem(index);
	return realized ? realized->GetDisplayText() : std::wstring{};
}

bool ItemsControl::ProcessTextSearchInput(
	const TextCompositionEventArgs& input)
{
	if (!GetIsTextSearchEnabled() || input.Text.empty()
		|| ItemCount() == 0) return false;

	const auto now = ::GetTickCount64();
	const auto timeout = static_cast<std::uint64_t>(
		::GetDoubleClickTime()) * 2u;
	if (_textSearchActive
		&& now - _textSearchLastInputTick > timeout)
		ResetTextSearch();

	const size_t count = ItemCount();
	const size_t start = _textSearchActive
		&& _textSearchMatchedIndex >= 0
		&& static_cast<size_t>(_textSearchMatchedIndex) < count
		? static_cast<size_t>(_textSearchMatchedIndex) : 0;
	const bool repeatedChunk = !_textSearchChunks.empty()
		&& TextEquals(
			_textSearchChunks.back(), input.Text, false);
	const auto newPrefix = _textSearchPrefix + input.Text;
	const bool caseSensitive = GetIsTextSearchCaseSensitive();
	size_t match = count;
	size_t fallback = count;

	for (size_t offset = 0; offset < count; ++offset)
	{
		const size_t index = (start + offset) % count;
		const auto itemText = GetTextSearchItemText(index);
		if (TextStartsWith(itemText, newPrefix, caseSensitive))
		{
			match = index;
			break;
		}
		if (repeatedChunk && offset != 0 && fallback == count
			&& TextStartsWith(
				itemText, _textSearchPrefix, caseSensitive))
			fallback = index;
	}

	const bool acceptedNewChunk = match != count;
	if (!acceptedNewChunk) match = fallback;
	if (match != count)
	{
		if (!_textSearchActive || match != start)
			OnTextSearchMatch(match);
		_textSearchMatchedIndex = static_cast<int>(match);
		if (acceptedNewChunk)
		{
			_textSearchPrefix = newPrefix;
			_textSearchChunks.push_back(input.Text);
		}
		_textSearchActive = true;
	}

	if (_textSearchActive)
		_textSearchLastInputTick = now;
	return match != count;
}

void ItemsControl::ProcessTextSearchKey(const InputReport& input)
{
	if (!GetIsTextSearchEnabled()
		|| input.Kind != InputReportKind::KeyDown
		|| input.Key != Key::Back
		|| !_textSearchActive) return;

	const auto now = ::GetTickCount64();
	const auto timeout = static_cast<std::uint64_t>(
		::GetDoubleClickTime()) * 2u;
	if (now - _textSearchLastInputTick > timeout)
	{
		ResetTextSearch();
		return;
	}
	if (_textSearchChunks.empty()) return;
	const auto chunkLength = _textSearchChunks.back().size();
	_textSearchChunks.pop_back();
	if (chunkLength <= _textSearchPrefix.size())
		_textSearchPrefix.resize(_textSearchPrefix.size() - chunkLength);
	else
		_textSearchPrefix.clear();
	_textSearchLastInputTick = now;
}

void ItemsControl::ResetTextSearch() noexcept
{
	_textSearchPrefix.clear();
	_textSearchChunks.clear();
	_textSearchMatchedIndex = -1;
	_textSearchLastInputTick = 0;
	_textSearchActive = false;
}

void ItemsControl::SetItemContainerStyle(std::wstring value)
{
	if (_itemContainerStyle == value) return;
	const auto previous = _itemContainerStyle;
	_itemContainerStyle = std::move(value);
	if (ApplyItemContainerStyle()) return;
	const auto error = _lastTemplateError.empty()
		? L"ItemContainerStyle 无法应用到项容器。" : _lastTemplateError;
	_itemContainerStyle = previous;
	(void)ApplyItemContainerStyle();
	_lastTemplateError = error;
}

void ItemsControl::SetGeneratedContainerInitializer(
	GeneratedContainerInitializer value)
{
	if (!_generatedContainerInitializer && !value)
		return;
	auto previous = std::move(_generatedContainerInitializer);
	_generatedContainerInitializer = std::move(value);
	if (RebuildGeneratedItems()) return;
	const auto error = _lastTemplateError;
	_generatedContainerInitializer = std::move(previous);
	(void)RebuildGeneratedItems();
	_lastTemplateError = error;
}

bool ItemsControl::InitializeGeneratedContainer(Control& container)
{
	if (!_generatedContainerInitializer) return true;
	std::wstring error;
	if (_generatedContainerInitializer(container, &error)) return true;
	_lastTemplateError = error.empty()
		? L"生成项容器的 XAML 类型身份初始化失败。" : std::move(error);
	return false;
}

bool ItemsControl::ApplyItemContainerStyle()
{
	auto apply = [this](Control* container)
	{
		if (!container) return true;
		cui::framework::StyleAccess::SetResourceKey(
			*container, _itemContainerStyle);
		return !cui::framework::StyleAccess::HasVisibleStyleRules(*container)
			|| cui::framework::StyleAccess::Refresh(*container, true);
	};
	if (!_itemsSource)
	{
		for (auto* item : _authoredItems)
			if (!apply(item)) return false;
		return true;
	}
	for (const auto& [index, realized] : GetRealizedItems())
	{
		(void)realized;
		if (!apply(GetGeneratedItem(index))) return false;
	}
	return true;
}

Control* ItemsControl::UnwrapGeneratedItem(Control* visual) noexcept
{
	if (auto* grouped = dynamic_cast<GroupedItemHost*>(visual))
		return grouped->Item();
	return visual;
}

bool ItemsControl::ClearGroupedItemLogicalParentPreservingOwnership(
	std::unique_ptr<Control>& visual)
{
	if (!visual || !dynamic_cast<GroupedItemHost*>(visual.get()))
		return true;
	return cui::framework::TreeAccess::InvokePreservingVisualOwnership(
		visual,
		[](Control& root)
		{
			auto* logicalItem = UnwrapGeneratedItem(&root);
			if (logicalItem && logicalItem != &root)
				cui::framework::TreeAccess::SetLogicalParent(
					*logicalItem, nullptr);
		});
}

Control* ItemsControl::GetGeneratedItem(size_t index) const noexcept
{
	if (!_itemsSource)
		return index < _authoredItems.size() ? _authoredItems[index] : nullptr;
	return UnwrapGeneratedItem(_generator.GetRealized(index));
}

size_t ItemsControl::GeneratedItemCount() const noexcept
{
	return _itemsSource ? _generator.RealizedCount() : _authoredItems.size();
}

size_t ItemsControl::ItemCount() const noexcept
{
	return _itemsSource ? _itemsSource.Get()->Count() : _authoredItems.size();
}

ItemsControl::AuthoredItemsUpdateScope::AuthoredItemsUpdateScope(
	ItemsControl& owner) noexcept
	: _owner(&owner)
{
	_owner->BeginAuthoredItemsUpdate();
}

ItemsControl::AuthoredItemsUpdateScope::AuthoredItemsUpdateScope(
	AuthoredItemsUpdateScope&& other) noexcept
	: _owner(std::exchange(other._owner, nullptr))
{
}

ItemsControl::AuthoredItemsUpdateScope&
ItemsControl::AuthoredItemsUpdateScope::operator=(
	AuthoredItemsUpdateScope&& other) noexcept
{
	if (this == &other) return *this;
	if (_owner) _owner->EndAuthoredItemsUpdate();
	_owner = std::exchange(other._owner, nullptr);
	return *this;
}

ItemsControl::AuthoredItemsUpdateScope::~AuthoredItemsUpdateScope()
{
	if (_owner) _owner->EndAuthoredItemsUpdate();
}

ItemsControl::AuthoredItemsUpdateScope
ItemsControl::DeferAuthoredItemsChanges() noexcept
{
	return AuthoredItemsUpdateScope(*this);
}

void ItemsControl::BeginAuthoredItemsUpdate() noexcept
{
	++_authoredItemsUpdateDepth;
}

void ItemsControl::EndAuthoredItemsUpdate() noexcept
{
	if (_authoredItemsUpdateDepth == 0) return;
	--_authoredItemsUpdateDepth;
	if (_authoredItemsUpdateDepth != 0 || !_authoredItemsChangedPending) return;
	_authoredItemsChangedPending = false;
	OnAuthoredItemsChanged();
	ConfigureVirtualHost();
	RequestLayout();
	InvalidateVisual();
}

void ItemsControl::NotifyAuthoredItemsChanged()
{
	ResetTextSearch();
	if (_authoredItemsUpdateDepth != 0)
	{
		_authoredItemsChangedPending = true;
		return;
	}
	OnAuthoredItemsChanged();
	ConfigureVirtualHost();
	RequestLayout();
	InvalidateVisual();
}

Control* ItemsControl::InsertItemControl(size_t index, Control* item)
{
	if (_migratingAuthoredItems)
		throw std::logic_error(
			"ItemsControl does not support authored-item mutation during ItemsHost migration");
	if (!item) throw std::invalid_argument("ItemsControl item is null");
	if (_itemsSource)
		throw std::logic_error(
			"ItemsControl Items and ItemsSource cannot both be populated");
	if (!_itemsHost) throw std::logic_error("ItemsControl has no ItemsHost");
	if (index > _authoredItems.size())
		throw std::out_of_range("ItemsControl item index is out of range");
	std::string itemError;
	if (!ValidateAuthoredItemControl(*item, itemError))
		throw std::invalid_argument(itemError.empty()
			? "ItemsControl rejected authored item" : itemError);
	_authoredItems.reserve(_authoredItems.size() + 1);
	const ControlWeakReference lifetime(item);
	bool structuralCommit = false;
	try
	{
		cui::framework::TreeAccess::InsertVisualChild(
			*_itemsHost, static_cast<int>(index), item, this,
			&structuralCommit);
	}
	catch (...)
	{
		auto* live = lifetime.Get();
		if (live && _itemsHost
			&& _itemsHost->IndexOfVisualChild(live) >= 0)
		{
			auto detached = _itemsHost->DetachVisualChild(live);
			if (detached.get() == live)
				(void)detached.release();
		}
		live = lifetime.Get();
		if (live && !live->GetVisualParent()
			&& live->GetLogicalParent() == this)
			cui::framework::TreeAccess::SetLogicalParent(
				*live, nullptr);
		throw;
	}
	auto* live = lifetime.Get();
	if (!live || !_itemsHost
		|| live->GetVisualParent() != _itemsHost
		|| _itemsHost->IndexOfVisualChild(live) < 0
		|| live->GetLogicalParent() != this)
		throw std::logic_error(
			structuralCommit
				? "ItemsControl item ownership changed during insertion"
				: "ItemsControl item insertion did not commit");
	_authoredItems.insert(_authoredItems.begin() + index, live);
	NotifyAuthoredItemsChanged();
	return live;
}

Control* ItemsControl::InsertItemControl(
	size_t index, std::unique_ptr<Control> item)
{
	if (_migratingAuthoredItems)
		throw std::logic_error(
			"ItemsControl does not support authored-item mutation during ItemsHost migration");
	if (!item) throw std::invalid_argument("ItemsControl item is null");
	if (_itemsSource)
		throw std::logic_error(
			"ItemsControl Items and ItemsSource cannot both be populated");
	if (!_itemsHost) throw std::logic_error("ItemsControl has no ItemsHost");
	if (index > _authoredItems.size())
		throw std::out_of_range("ItemsControl item index is out of range");
	std::string itemError;
	if (!ValidateAuthoredItemControl(*item, itemError))
		throw std::invalid_argument(itemError.empty()
			? "ItemsControl rejected authored item" : itemError);
	_authoredItems.reserve(_authoredItems.size() + 1);

	auto* raw = item.get();
	const ControlWeakReference lifetime(raw);
	try
	{
		(void)cui::framework::TreeAccess::
			InsertOwnedVisualChildPreserving(
				*_itemsHost, static_cast<int>(index),
				item, this);
	}
	catch (...)
	{
		auto* live = lifetime.Get();
		if (live && _itemsHost
			&& _itemsHost->IndexOfVisualChild(live) >= 0)
		{
			bool ownershipCommit = false;
			std::exception_ptr ignoredNotificationError;
			auto recovered =
				cui::framework::TreeAccess::DetachVisualChild(
					*_itemsHost, live, &ownershipCommit,
					&ignoredNotificationError);
			if (recovered)
				item.reset(recovered.release());
		}
		throw;
	}
	auto* live = lifetime.Get();
	if (!live || !_itemsHost
		|| live->GetVisualParent() != _itemsHost
		|| _itemsHost->IndexOfVisualChild(live) < 0
		|| live->GetLogicalParent() != this)
		throw std::logic_error(
			"ItemsControl owned item was transferred during insertion");
	_authoredItems.insert(_authoredItems.begin() + index, live);
	NotifyAuthoredItemsChanged();
	return live;
}

Control* ItemsControl::AddItemControl(std::unique_ptr<Control> item)
{
	return InsertItemControl(_authoredItems.size(), std::move(item));
}

Control* ItemsControl::AdoptItemControl(Control* item)
{
	return InsertItemControl(_authoredItems.size(), item);
}

std::unique_ptr<Control> ItemsControl::DetachItemControlAt(size_t index)
{
	if (_migratingAuthoredItems)
		throw std::logic_error(
			"ItemsControl does not support authored-item mutation during ItemsHost migration");
	if (index >= _authoredItems.size() || !_itemsHost) return {};
	auto* raw = _authoredItems[index];
	const ControlWeakReference lifetime(raw);
	auto* sourceHost = _itemsHost;
	auto result = sourceHost->DetachVisualChild(raw);
	auto* live = lifetime.Get();
	if (live && live->GetVisualParent() == sourceHost
		&& sourceHost->IndexOfVisualChild(live) >= 0)
		return {};
	// The visual removal has committed even when a callback transferred or
	// destroyed the item and no owner token can be returned.
	_authoredItems.erase(_authoredItems.begin() + index);
	std::exception_ptr parentError;
	if (result)
	{
		try
		{
			(void)cui::framework::TreeAccess::
				SetLogicalParentPreservingOwnership(
					result, nullptr, &parentError);
		}
		catch (...)
		{
			parentError = std::current_exception();
		}
	}
	try { NotifyAuthoredItemsChanged(); }
	catch (...)
	{
		if (!parentError) parentError = std::current_exception();
	}
	if (parentError)
		std::rethrow_exception(parentError);
	return result;
}

std::unique_ptr<Control> ItemsControl::DetachItemControl(Control* item)
{
	const auto found = std::find(_authoredItems.begin(), _authoredItems.end(), item);
	return found == _authoredItems.end() ? std::unique_ptr<Control>{}
		: DetachItemControlAt(static_cast<size_t>(
			std::distance(_authoredItems.begin(), found)));
}

bool ItemsControl::RemoveItemControlAt(size_t index)
{
	return static_cast<bool>(DetachItemControlAt(index));
}

bool ItemsControl::RemoveItemControl(Control* item)
{
	return static_cast<bool>(DetachItemControl(item));
}

bool ItemsControl::MoveItemControl(size_t oldIndex, size_t newIndex)
{
	if (_migratingAuthoredItems)
		throw std::logic_error(
			"ItemsControl does not support authored-item mutation during ItemsHost migration");
	if (oldIndex >= _authoredItems.size() || newIndex >= _authoredItems.size())
		return false;
	if (oldIndex == newIndex) return true;
	if (!_itemsHost) return false;
	_itemsHost->MoveVisualChild(
		static_cast<int>(oldIndex), static_cast<int>(newIndex));
	auto* item = _authoredItems[oldIndex];
	_authoredItems.erase(_authoredItems.begin() + oldIndex);
	_authoredItems.insert(_authoredItems.begin() + newIndex, item);
	NotifyAuthoredItemsChanged();
	return true;
}

void ItemsControl::ClearItemControls()
{
	if (_migratingAuthoredItems)
		throw std::logic_error(
			"ItemsControl does not support authored-item mutation during ItemsHost migration");
	std::vector<std::unique_ptr<Control>> removed;
	removed.reserve(_authoredItems.size());
	auto update = DeferAuthoredItemsChanges();
	while (!_authoredItems.empty())
		removed.push_back(DetachItemControlAt(_authoredItems.size() - 1));
}

Control* ItemsControl::GetAuthoredItem(size_t index) const noexcept
{
	return index < _authoredItems.size() ? _authoredItems[index] : nullptr;
}

bool ItemsControl::IsGroupingActive() const noexcept
{
	return _virtualGroupingActive;
}

ItemsControl::PreparedGroupHeaders ItemsControl::BuildGroupHeaders(
	size_t index,
	const BindingSourceReference& item)
{
	PreparedGroupHeaders result;
	if (!IsGroupingActive()) return result;
	const auto first = std::lower_bound(
		_cachedGroupDefinitions.begin(), _cachedGroupDefinitions.end(), index,
		[](const BindingListGroup& group, size_t itemIndex)
		{ return group.StartIndex < itemIndex; });
	for (auto current = first;
		current != _cachedGroupDefinitions.end()
			&& current->StartIndex == index; ++current)
	{
		const auto& group = *current;
		const size_t cachedIndex = static_cast<size_t>(
			current - _cachedGroupDefinitions.begin());
		const bool isBottomLevel = cachedIndex
			< _cachedGroupBottomLevels.size()
			? _cachedGroupBottomLevels[cachedIndex] != 0 : true;
		auto groupItems = std::make_shared<ReadOnlyBindingListSlice>(
			_itemsSource, group.StartIndex, group.ItemCount);
		auto context = std::make_shared<ObservableObject>();
		auto aggregates = std::make_shared<ObservableObject>();
		for (const auto& [name, value] : group.Aggregates)
			(void)aggregates->DefineProperty(name, value, true, false, true);
		(void)context->DefineProperty(L"Key", group.Key.ToString(), true, false, true);
		(void)context->DefineProperty(L"Name", group.Key.ToString(), true, false, true);
		(void)context->DefineProperty(L"PropertyName", group.PropertyName,
			true, false, true);
		(void)context->DefineProperty(L"Level", static_cast<int>(group.Level),
			true, false, true);
		(void)context->DefineProperty(L"StartIndex",
			static_cast<long long>(group.StartIndex), true, false, true);
		(void)context->DefineProperty(L"ItemCount",
			static_cast<long long>(group.ItemCount), true, false, true);
		(void)context->DefineProperty(L"IsBottomLevel", isBottomLevel,
			true, false, true);
		(void)context->DefineProperty(L"FirstItem", item, true, false, true);
		(void)context->DefineProperty(L"Items",
			BindingListReference(groupItems), true, false, true);
		(void)context->DefineProperty(L"Aggregates",
			BindingSourceReference(aggregates), true, false, true);
		BindingSourceReference contextReference(context);
		std::unique_ptr<Control> header;
		if (_groupStyle.Get()->HeaderTemplate)
			header = _groupStyle.Get()->HeaderTemplate.Get()->Build(
				contextReference, index, &_lastTemplateError);
		else
		{
			auto label = std::make_unique<Label>();
			label->Text = group.Key.ToString();
			header = std::move(label);
		}
		if (!header)
		{
			if (_lastTemplateError.empty())
				_lastTemplateError = L"GroupStyle HeaderTemplate 未生成视觉根。";
			return {};
		}
		result.Visuals.push_back(std::move(header));
		result.Contexts.push_back(std::move(contextReference));
	}
	return result;
}

void ItemsControl::RefreshGroupHeaders()
{
	_lastTemplateError.clear();
	RefreshVirtualGroupHeaderMetadata();
	const bool active = IsGroupingActive();
	if (active
		&& ((!IsVirtualizing() && _generator.RealizedCount() != ItemCount())
			|| (IsVirtualizing() && ItemCount() != 0
				&& _generator.RealizedCount() == 0)))
	{
		(void)RebuildGeneratedItems();
		return;
	}
	for (const auto& [index, realized] : _generator.RealizedItems())
	{
		const bool wrapped = dynamic_cast<GroupedItemHost*>(
			realized.Visual) != nullptr;
		if (wrapped != active)
		{
			(void)RebuildGeneratedItems();
			return;
		}
	}
	if (!active)
	{
		ConfigureVirtualHost();
		if (_itemsHost)
		{
			_itemsHost->InvalidateLayout();
			RequestLayout();
			InvalidateVisual();
		}
		return;
	}
	struct Pending final
	{
		GroupedItemHost* Host = nullptr;
		std::vector<std::unique_ptr<Control>> Headers;
		std::vector<BindingSourceReference> Contexts;
	};
	std::vector<Pending> pending;
	pending.reserve(_generator.RealizedCount());
	for (const auto& [index, realized] : _generator.RealizedItems())
	{
		BindingSourceReference item;
		if (!_itemsSource.Get()->TryGetItem(index, item) || !item)
		{
			_lastTemplateError = L"分组 ItemsSource 无法读取项。";
			return;
		}
		auto headers = BuildGroupHeaders(index, item);
		if (!_lastTemplateError.empty()) return;
		pending.push_back({
			static_cast<GroupedItemHost*>(realized.Visual),
			std::move(headers.Visuals), std::move(headers.Contexts) });
	}
	for (auto& item : pending)
		item.Host->SetHeaders(
			std::move(item.Headers), std::move(item.Contexts),
			IsVirtualizing() ? VirtualizedGroupHeaderEstimate : 0.0f,
			IsVirtualizing() ? GetVirtualizedItemHeight() : 0.0f);
	ConfigureVirtualHost();
	_itemsHost->InvalidateLayout();
	RequestLayout();
	InvalidateVisual();
}

bool ItemsControl::PrepareGeneratedItem(
	size_t index,
	PreparedItem& output,
	bool allowRecycle,
	std::span<const size_t> crossIndexRecycleReservations,
	std::span<const CrossIndexRecycleCandidate> crossIndexRecycleCandidates)
{
	const ControlWeakReference ownerLifetime(this);
	RefreshVirtualGroupHeaderMetadata();
	output = {};
	output.Index = index;
	ItemContainerGenerator::RecycledItem recycled;
	if (allowRecycle && _generator.TakeRecycled(index, recycled))
	{
		output.Visual = std::move(recycled.Visual);
		output.Observation = std::move(recycled.Observation);
		output.WasRecycled = true;
		auto* grouped = dynamic_cast<GroupedItemHost*>(output.Visual.get());
		if (!IsGroupingActive())
		{
			if (grouped) output.Visual = grouped->TakeItem();
			return output.Visual != nullptr;
		}
		BindingSourceReference recycledItem;
		if (!_itemsSource || !_itemsSource.Get()->TryGetItem(index, recycledItem)
			|| !recycledItem)
		{
			_lastTemplateError = L"ItemsSource 无法读取回收项索引 "
				+ std::to_wstring(index) + L"。";
			return false;
		}
		auto headers = BuildGroupHeaders(index, recycledItem);
		if (!_lastTemplateError.empty()) return false;
		if (grouped)
			grouped->SetHeaders(
				std::move(headers.Visuals), std::move(headers.Contexts),
				IsVirtualizing() ? VirtualizedGroupHeaderEstimate : 0.0f,
				IsVirtualizing() ? GetVirtualizedItemHeight() : 0.0f);
		else
		{
			auto host = std::make_unique<GroupedItemHost>(
				std::move(output.Visual), this);
			host->SetHeaders(
				std::move(headers.Visuals), std::move(headers.Contexts),
				IsVirtualizing() ? VirtualizedGroupHeaderEstimate : 0.0f,
				IsVirtualizing() ? GetVirtualizedItemHeight() : 0.0f);
			output.Visual = std::move(host);
		}
		return true;
	}
	if (allowRecycle && !crossIndexRecycleCandidates.empty()
		&& !IsGroupingActive())
	{
		const auto source = _itemsSource.Shared();
		const size_t sourceCount = _generator.SourceCount();
		const size_t generatedRevision = _generatedItemsRevision;
		size_t oldIndex = 0;
		bool donorFound = false;
		for (const auto& candidate : crossIndexRecycleCandidates)
		{
			if (std::binary_search(
				crossIndexRecycleReservations.begin(),
				crossIndexRecycleReservations.end(), candidate.Index))
				continue;
			auto* live = dynamic_cast<ItemsControl*>(ownerLifetime.Get());
			if (!live || live->_itemsSource.Shared() != source
				|| live->_generator.SourceCount() != sourceCount
				|| live->_generatedItemsRevision != generatedRevision)
				return false;
			auto& pool = live->_generator.RecycledItems();
			auto donor = pool.find(candidate.Index);
			if (donor == pool.end() || !donor->second.Visual
				|| donor->second.Visual.get() != candidate.Visual)
				continue;
			const ControlWeakReference donorLifetime(candidate.Visual);
			const bool compatible =
				live->CanRecycleGeneratedItemAcrossIndices(
					*candidate.Visual, candidate.Index);
			live = dynamic_cast<ItemsControl*>(ownerLifetime.Get());
			if (!live || live->_itemsSource.Shared() != source
				|| live->_generator.SourceCount() != sourceCount
				|| live->_generatedItemsRevision != generatedRevision)
				return false;
			if (!compatible || donorLifetime.Get() != candidate.Visual)
				continue;
			auto& validatedPool = live->_generator.RecycledItems();
			donor = validatedPool.find(candidate.Index);
			if (donor == validatedPool.end() || !donor->second.Visual
				|| donor->second.Visual.get() != candidate.Visual)
				continue;
			oldIndex = donor->first;
			recycled = std::move(donor->second);
			validatedPool.erase(donor);
			donorFound = true;
			break;
		}
		if (donorFound)
		{
			BindingSourceReference item;
			const bool itemRead = _itemsSource
				&& _itemsSource.Get()->TryGetItem(index, item) && item;
			auto* live = dynamic_cast<ItemsControl*>(ownerLifetime.Get());
			if (!live || live->_itemsSource.Shared() != source
				|| live->_generator.SourceCount() != sourceCount
				|| live->_generatedItemsRevision != generatedRevision)
				return false;
			if (!itemRead)
			{
				live->_generator.StoreRecycled(oldIndex, std::move(recycled));
				live->_lastTemplateError = L"ItemsSource 无法读取跨索引回收项 "
					+ std::to_wstring(index) + L"。";
				return false;
			}
			std::wstring error;
			const bool rebound = live->TryRebindGeneratedItemAcrossIndices(
				*recycled.Visual, oldIndex, index, item,
				recycled.Observation, &error);
			live = dynamic_cast<ItemsControl*>(ownerLifetime.Get());
			if (!live || live->_itemsSource.Shared() != source
				|| live->_generator.SourceCount() != sourceCount
				|| live->_generatedItemsRevision != generatedRevision)
				return false;
			if (!rebound)
			{
				if (!error.empty()) live->_lastTemplateError = std::move(error);
				else if (live->_lastTemplateError.empty())
					live->_lastTemplateError = L"项容器跨索引回收失败。";
				// The donor was detached before this realization transaction began.
				// Discarding a partially rebound cache entry cannot mutate the
				// currently committed visual tree.
				return false;
			}
			output.Visual = std::move(recycled.Visual);
			output.Observation = std::move(recycled.Observation);
			output.WasRecycled = true;
			return output.Visual != nullptr;
		}
	}

	BindingSourceReference item;
	if (!_itemsSource || !_itemsSource.Get()->TryGetItem(index, item) || !item)
	{
		_lastTemplateError = L"ItemsSource 无法读取索引 "
			+ std::to_wstring(index) + L"。";
		return false;
	}
	auto visual = BuildGeneratedItem(item, index, output.Observation);
	if (!ownerLifetime.Get()) return false;
	if (!visual)
	{
		if (_lastTemplateError.empty())
			_lastTemplateError = L"项容器未生成视觉根。";
		return false;
	}
	bool initialized = false;
	if (!cui::framework::TreeAccess::InvokePreservingVisualOwnership(
		visual,
		[&](Control& container)
		{
			initialized = InitializeGeneratedContainer(container);
		}))
		throw std::logic_error(
			"generated container ownership changed during initialization");
	if (!initialized) return false;
	if (IsGroupingActive())
	{
		auto headers = BuildGroupHeaders(index, item);
		if (!_lastTemplateError.empty()) return false;
		auto grouped = std::make_unique<GroupedItemHost>(
			std::move(visual), this);
		grouped->SetHeaders(
			std::move(headers.Visuals), std::move(headers.Contexts),
			IsVirtualizing() ? VirtualizedGroupHeaderEstimate : 0.0f,
			IsVirtualizing() ? GetVirtualizedItemHeight() : 0.0f);
		visual = std::move(grouped);
	}
	output.Visual = std::move(visual);
	return true;
}

std::unique_ptr<Control> ItemsControl::BuildGeneratedItem(
	const BindingSourceReference& item,
	size_t index,
	BindingPathObservation& observation)
{
	observation = {};
	auto presenter = std::make_unique<ContentPresenter>();
	(void)cui::framework::DependencyPropertyAccess::SetValue(
		*presenter, Control::VerticalAlignmentProperty(),
		BindingValue(VerticalAlignment::Top),
		DependencyPropertyValueSource::Theme);
	cui::framework::StyleAccess::SetResourceKey(
		*presenter, _itemContainerStyle);
	presenter->SetContentTypeToken(_itemTemplate
		? _itemTemplate.Get()->GetDataTypeToken()
		: (_itemsSource ? _itemsSource.Get()->GetItemTypeToken()
			: DataTypeToken{}));
	presenter->SetCompiledDisplayMemberPath(_compiledDisplayMemberPath);
#if CUI_ENABLE_DYNAMIC_XAML
	ApplyAuthoredGeneratedItemProjection(*presenter);
#endif
	presenter->SetContentTemplate(_itemTemplate);
	try
	{
		presenter->SetContent(BindingValue(item));
	}
	catch (...)
	{
		// ContentPresenter owns the template diagnostic buffer.  Preserve it on
		// the ItemsControl transaction before the temporary presenter unwinds.
		_lastTemplateError = presenter->LastTemplateError();
		throw;
	}
	if (!presenter->LastTemplateError().empty())
	{
		_lastTemplateError = presenter->LastTemplateError();
		return {};
	}
	return WrapGeneratedItem(std::move(presenter), item, index);
}

void ItemsControl::AttachPreparedItem(PreparedItem&& item)
{
	if (!_itemsHost)
		throw std::logic_error("ItemsControl has no ItemsHost");
	if (!item.Visual)
		throw std::invalid_argument(
			"ItemsControl prepared item has no visual");
	const auto index = item.Index;
	auto* requestedHost = _itemsHost;
	auto* requestedVisual = item.Visual.get();
	const bool groupedVisual =
		dynamic_cast<GroupedItemHost*>(requestedVisual) != nullptr;
	auto* requestedLogicalItem = groupedVisual
		? UnwrapGeneratedItem(requestedVisual) : requestedVisual;
	const ControlWeakReference hostLifetime(requestedHost);
	const ControlWeakReference visualLifetime(requestedVisual);
	const ControlWeakReference logicalItemLifetime(requestedLogicalItem);
	auto validateAttachment = [&]() -> std::pair<Panel*, Control*>
	{
		auto* liveHost = dynamic_cast<Panel*>(hostLifetime.Get());
		auto* liveVisual = visualLifetime.Get();
		auto* liveLogicalItem = logicalItemLifetime.Get();
		const bool rootOwned = liveHost && _itemsHost == liveHost
			&& liveVisual
			&& liveVisual->GetVisualParent() == liveHost
			&& liveHost->IndexOfVisualChild(liveVisual) >= 0;
		const bool logicalOwned = groupedVisual
			? liveLogicalItem
				&& liveLogicalItem != liveVisual
				&& UnwrapGeneratedItem(liveVisual) == liveLogicalItem
				&& liveLogicalItem->GetLogicalParent() == this
			: liveLogicalItem == liveVisual
				&& liveVisual
				&& liveVisual->GetLogicalParent() == this;
		if (!rootOwned || !logicalOwned)
			throw std::logic_error(
				"ItemsControl prepared item ownership did not commit");
		return { liveHost, liveVisual };
	};

	try
	{
		(void)cui::framework::TreeAccess::
			InsertOwnedVisualChildPreserving(
				*requestedHost, requestedHost->VisualChildCount(),
				item.Visual, groupedVisual ? nullptr : this);
		auto [liveHost, liveVisual] = validateAttachment();

		// Generated containers may be realized after the document-wide style
		// pass. Re-resolve only after the live inheritance route exists.
		if (!cui::framework::StyleAccess::Theme(*liveVisual))
			if (const auto theme =
				cui::framework::StyleAccess::Theme(*this))
				(void)cui::framework::StyleAccess::SetTheme(
					*liveVisual, std::move(theme), true);
		std::tie(liveHost, liveVisual) = validateAttachment();
		if (!cui::framework::StyleAccess::DocumentStyles(*liveVisual))
			if (const auto styles =
				cui::framework::StyleAccess::DocumentStyles(*this))
				(void)cui::framework::StyleAccess::SetDocumentStyles(
					*liveVisual, std::move(styles), true);
		std::tie(liveHost, liveVisual) = validateAttachment();
		if (groupedVisual)
		{
			// GroupedItemHost is only a visual wrapper.  Its Item deliberately
			// keeps ItemsControl as its logical/inheritance parent, so a recursive
			// environment install on the wrapper cannot reach the real generated
			// container.  Publish the same late-realization environment to that
			// logical subtree as well (DataGridRow -> cells -> column elements).
			auto* liveLogicalItem = logicalItemLifetime.Get();
			if (!liveLogicalItem || liveLogicalItem == liveVisual)
				throw std::logic_error(
					"ItemsControl grouped logical item was lost");
			const auto inheritedTheme =
				cui::framework::StyleAccess::Theme(*this);
			const auto inheritedStyles =
				cui::framework::StyleAccess::DocumentStyles(*this);
			const auto theme = inheritedTheme ? inheritedTheme
				: cui::framework::StyleAccess::Theme(*liveLogicalItem);
			const auto styles = inheritedStyles ? inheritedStyles
				: cui::framework::StyleAccess::DocumentStyles(*liveLogicalItem);
			// A recycled grouped container can still carry the environment inherited
			// from its previous owner. Compare and install both sides atomically so a
			// theme switch cannot leave stale Theme/Document halves in that subtree.
			// When the owner has no counterpart, preserve an explicitly staged item
			// side just like ordinary visual-child attachment does.
			if ((cui::framework::StyleAccess::Theme(*liveLogicalItem) != theme
				|| cui::framework::StyleAccess::DocumentStyles(*liveLogicalItem)
					!= styles)
				&& !cui::framework::StyleAccess::SetEnvironment(
					*liveLogicalItem, theme, styles, true))
				throw std::runtime_error(
					"ItemsControl grouped style environment failed");
			std::tie(liveHost, liveVisual) = validateAttachment();
		}
		if (auto* virtualHost =
			dynamic_cast<VirtualizingItemsHost*>(liveHost))
		{
			virtualHost->RegisterItem(liveVisual, index);
			std::tie(liveHost, liveVisual) = validateAttachment();
			virtualHost = dynamic_cast<VirtualizingItemsHost*>(liveHost);
			if (!virtualHost || virtualHost->IndexOf(liveVisual) != index)
				throw std::logic_error(
					"ItemsControl virtual item registration did not commit");
		}
		_generator.StoreRealized(
			index, liveVisual, std::move(item.Observation));
		if (_generator.GetRealized(index) != visualLifetime.Get())
			throw std::logic_error(
				"ItemsControl generator realization did not commit");
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		if (_generator.GetRealized(index) == requestedVisual)
			(void)_generator.TakeRealized(index);

		auto* liveHost = dynamic_cast<Panel*>(hostLifetime.Get());
		auto* liveVisual = visualLifetime.Get();
		if (auto* virtualHost =
			dynamic_cast<VirtualizingItemsHost*>(liveHost))
			virtualHost->UnregisterItem(requestedVisual);
		if (liveHost && liveVisual
			&& liveVisual->GetVisualParent() == liveHost
			&& liveHost->IndexOfVisualChild(liveVisual) >= 0)
		{
			try
			{
				bool ownershipCommit = false;
				std::exception_ptr ignoredNotificationError;
				auto recovered =
					cui::framework::TreeAccess::DetachVisualChild(
						*liveHost, liveVisual, &ownershipCommit,
						&ignoredNotificationError);
				if (recovered)
				{
					try
					{
						(void)ClearGroupedItemLogicalParentPreservingOwnership(
							recovered);
						std::exception_ptr ignoredParentError;
						if (recovered)
							(void)cui::framework::TreeAccess::
								SetLogicalParentPreservingOwnership(
									recovered, nullptr,
									&ignoredParentError);
					}
					catch (...) {}
				}
			}
			catch (...) {}
		}
		std::rethrow_exception(originalError);
	}
}

void ItemsControl::ReorderRealizedChildren()
{
	if (!_itemsHost || IsVirtualizing()
		|| _generator.RealizedCount() < 2) return;
	struct RealizedOrderEntry final
	{
		size_t Index = 0;
		Control* Identity = nullptr;
		ControlWeakReference Lifetime;
	};

	auto* requestedHost = _itemsHost;
	const ControlWeakReference hostLifetime(requestedHost);
	std::vector<RealizedOrderEntry> desired;
	std::vector<ControlWeakReference> previousOrder;
	std::unordered_set<Control*> identities;
	desired.reserve(_generator.RealizedCount());
	previousOrder.reserve(
		static_cast<size_t>(requestedHost->VisualChildCount()));
	identities.reserve(_generator.RealizedCount());
	for (const auto& [index, item] : _generator.RealizedItems())
	{
		auto* visual = item.Visual;
		if (!visual || !identities.insert(visual).second)
			throw std::logic_error(
				"ItemsControl generator contains an invalid realized identity");
		const ControlWeakReference lifetime(visual);
		if (lifetime.Get() != visual
			|| visual->GetVisualParent() != requestedHost
			|| requestedHost->IndexOfVisualChild(visual) < 0)
			throw std::logic_error(
				"ItemsControl generator is not owned by its ItemsHost");
		desired.push_back({ index, visual, lifetime });
	}
	for (auto* child : requestedHost->GetVisualChildrenView())
		previousOrder.emplace_back(child);
	if (previousOrder.size() != desired.size())
		throw std::logic_error(
			"ItemsControl ItemsHost contains untracked realized children");

	try
	{
		for (size_t targetIndex = 0;
			targetIndex < desired.size(); ++targetIndex)
		{
			auto* liveHost = dynamic_cast<Panel*>(hostLifetime.Get());
			auto* liveVisual = desired[targetIndex].Lifetime.Get();
			if (!liveHost || _itemsHost != liveHost
				|| !liveVisual
				|| liveVisual->GetVisualParent() != liveHost)
				throw std::logic_error(
					"ItemsControl realized ownership changed during reorder");
			const int currentIndex =
				liveHost->IndexOfVisualChild(liveVisual);
			if (currentIndex < 0)
				throw std::logic_error(
					"ItemsControl realized child escaped during reorder");
			if (currentIndex != static_cast<int>(targetIndex)
				&& !liveHost->MoveVisualChild(
					currentIndex, static_cast<int>(targetIndex)))
				throw std::logic_error(
					"ItemsControl realized child could not be reordered");
		}

		auto* liveHost = dynamic_cast<Panel*>(hostLifetime.Get());
		if (!liveHost || _itemsHost != liveHost
			|| liveHost->VisualChildCount()
			!= static_cast<int>(desired.size()))
			throw std::logic_error(
				"ItemsControl ItemsHost changed during reorder");
		const auto children = liveHost->GetVisualChildrenView();
		for (size_t index = 0; index < desired.size(); ++index)
		{
			auto* live = desired[index].Lifetime.Get();
			if (!live || children[index] != live
				|| live->GetVisualParent() != liveHost)
				throw std::logic_error(
					"ItemsControl realized order did not commit");
		}
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		// Moving never relinquishes ownership. Restore the old order in place
		// when all captured identities still belong to the same host.
		try
		{
			auto* liveHost = dynamic_cast<Panel*>(hostLifetime.Get());
			bool canRestore = liveHost && _itemsHost == liveHost
				&& liveHost->VisualChildCount()
				== static_cast<int>(previousOrder.size());
			for (const auto& reference : previousOrder)
			{
				auto* child = reference.Get();
				canRestore = canRestore && child
					&& child->GetVisualParent() == liveHost
					&& liveHost->IndexOfVisualChild(child) >= 0;
			}
			if (canRestore)
			{
				for (size_t targetIndex = 0;
					targetIndex < previousOrder.size(); ++targetIndex)
				{
					auto* child = previousOrder[targetIndex].Get();
					const int currentIndex =
						liveHost->IndexOfVisualChild(child);
					if (currentIndex != static_cast<int>(targetIndex))
						(void)liveHost->MoveVisualChild(
							currentIndex,
							static_cast<int>(targetIndex));
				}
			}
		}
		catch (...) {}

		// A reentrant collection observer may still have transferred or
		// destroyed a visual. Remove only those invalid identities so the
		// generator never publishes stale raw pointers.
		auto* liveHost = dynamic_cast<Panel*>(hostLifetime.Get());
		for (const auto& entry : desired)
		{
			if (_generator.GetRealized(entry.Index) != entry.Identity)
				continue;
			auto* live = entry.Lifetime.Get();
			if (!liveHost || _itemsHost != liveHost || !live
				|| live->GetVisualParent() != liveHost
				|| liveHost->IndexOfVisualChild(live) < 0)
				(void)_generator.TakeRealized(entry.Index);
		}
		std::rethrow_exception(originalError);
	}
}

void ItemsControl::ClearRealizedItems(bool keepForRecycle)
{
	const ControlWeakReference ownerLifetime(this);
	if (!_itemsHost)
	{
		_generator.ClearRealized();
		AdvanceGeneratedItemsRevision();
		return;
	}
	std::vector<size_t> indices;
	indices.reserve(_generator.RealizedCount());
	for (const auto& [index, item] : _generator.RealizedItems())
	{
		(void)item;
		indices.push_back(index);
	}
	for (const auto index : indices)
	{
		auto* visual = _generator.GetRealized(index);
		if (visual)
		{
			if (auto* container = UnwrapGeneratedItem(visual))
				OnGeneratedItemClearing(*container);
			if (!ownerLifetime.Get()) return;
			if (_generator.GetRealized(index) != visual) continue;
		}
		auto item = _generator.TakeRealized(index);
		visual = item.Visual;
		if (auto* virtualHost = dynamic_cast<VirtualizingItemsHost*>(_itemsHost))
			virtualHost->UnregisterItem(visual);
		auto detached = _itemsHost->DetachVisualChild(visual);
		if (detached)
		{
			(void)ClearGroupedItemLogicalParentPreservingOwnership(
				detached);
			std::exception_ptr parentError;
			if (detached)
				(void)cui::framework::TreeAccess::
					SetLogicalParentPreservingOwnership(
						detached, nullptr, &parentError);
			if (parentError)
				std::rethrow_exception(parentError);
		}
		if (keepForRecycle && detached)
			_generator.StoreRecycled(index, {
				std::move(detached), std::move(item.Observation) });
	}
	AdvanceGeneratedItemsRevision();
}

std::pair<size_t, size_t> ItemsControl::VirtualRangeForViewport() const noexcept
{
	auto* scroll = ItemsScrollOwner();
	if (!scroll) return ShouldRealizeVirtualItemsWithoutViewport()
		? std::pair<size_t, size_t>{ 0, ItemCount() }
		: std::pair<size_t, size_t>{ 0, 0 };
	return VirtualRangeForOffset(scroll->VerticalOffset);
}

std::pair<size_t, size_t> ItemsControl::VirtualRangeForOffset(
	double offset) const noexcept
{
	const size_t count = ItemCount();
	if (count == 0) return { 0, 0 };
	const auto& panel = EffectiveItemsPanel();
	auto* self = const_cast<ItemsControl*>(this);
	self->ConfigureVirtualHost();
	const auto* host = dynamic_cast<const VirtualizingItemsHost*>(_itemsHost);
	if (!host || host->ContentHeight() <= 0.0) return { 0, count };
	auto* scroll = ItemsScrollOwner();
	if (!scroll) return ShouldRealizeVirtualItemsWithoutViewport()
		? std::pair<size_t, size_t>{ 0, count }
		: std::pair<size_t, size_t>{ 0, 0 };
	const auto size = const_cast<ScrollViewer*>(scroll)->GetActualSizeDip();
	const double viewport = std::isfinite(size.height)
		? (std::max)(1.0, static_cast<double>(size.height)) : 1.0;
	const bool visibleOnlyThumbRange =
		scroll->_draggingVerticalScrollBar
		&& UseVisibleOnlyRangeDuringVerticalThumbDrag();
	double cache = visibleOnlyThumbRange ? 0.0
		: static_cast<double>(panel.CacheLength) * viewport;
	if (!std::isfinite(cache))
		cache = (std::numeric_limits<double>::max)();
	const double contentHeight = host->ContentHeight();
	const double normalizedOffset = std::isnan(offset) || offset <= 0.0
		? 0.0 : !std::isfinite(offset) || offset >= contentHeight
			? contentHeight : offset;
	const double firstOffset = normalizedOffset > cache
		? normalizedOffset - cache : 0.0;
	const size_t first = host->IndexAtOffset(firstOffset);
	double forward = viewport + cache;
	if (!std::isfinite(forward))
		forward = (std::numeric_limits<double>::max)();
	const double remaining = contentHeight - normalizedOffset;
	const double endOffset = forward >= remaining
		? contentHeight : normalizedOffset + forward;
	const size_t last = endOffset >= contentHeight
		? count : (std::min)(count, host->IndexAtOffset(endOffset) + 1);
	return { first, last };
}

bool ItemsControl::RealizeVirtualRange(
	size_t first, size_t last, bool localLayoutForScroll)
{
	if (!IsVirtualizing() || !_itemsHost || _realizingViewport) return true;
	const ControlWeakReference ownerLifetime(this);
	const size_t count = ItemCount();
	first = (std::min)(first, count);
	last = (std::clamp)(last, first, count);
	auto* virtualHost = dynamic_cast<VirtualizingItemsHost*>(_itemsHost);
	const bool previousLocalLayout = virtualHost
		? virtualHost->SetLocalLayoutInvalidation(localLayoutForScroll)
		: false;
	const ControlWeakReference hostLifetime(virtualHost);
	ItemsScopeExit localLayoutGuard([hostLifetime, previousLocalLayout]
	{
		if (auto* host = dynamic_cast<VirtualizingItemsHost*>(
			hostLifetime.Get()))
			(void)host->SetLocalLayoutInvalidation(previousLocalLayout);
	});
	_realizingViewport = true;
	ItemsScopeExit realizingGuard([ownerLifetime]
	{
		if (auto* owner = dynamic_cast<ItemsControl*>(ownerLifetime.Get()))
			owner->_realizingViewport = false;
	});
	auto& recycleCandidates = _virtualRecycleCandidatesScratch;
	recycleCandidates.clear();
	recycleCandidates.reserve(_generator.RecycledItems().size());
	for (const auto& [index, item] : _generator.RecycledItems())
		recycleCandidates.push_back({ index, item.Visual.get() });
	auto& additionIndices = _virtualAdditionIndicesScratch;
	additionIndices.clear();
	additionIndices.reserve(last - first);
	for (size_t index = first; index < last; ++index)
		if (!_generator.ContainsRealized(index))
			additionIndices.push_back(index);
	// Move containers that leave the range directly onto entering indices while
	// they stay attached. Only the remainder uses the detach/recycle-pool path,
	// which re-propagates inheritance, styles and window ownership through every
	// descendant and changes the retained scene topology.
	size_t reindexedInPlace = 0;
	if (virtualHost && !additionIndices.empty() && !IsGroupingActive())
	{
		const auto inPlaceSource = _itemsSource.Shared();
		const size_t inPlaceSourceCount = _generator.SourceCount();
		const size_t inPlaceRevision = _generatedItemsRevision;
		auto& leaving = _virtualInPlaceLeavingScratch;
		leaving.clear();
		for (const auto& [index, realized] : _generator.RealizedItems())
		{
			(void)realized;
			if (index < first || index >= last) leaving.push_back(index);
		}
		size_t leavingCursor = 0;
		size_t write = 0;
		for (size_t read = 0; read < additionIndices.size(); ++read)
		{
			const size_t target = additionIndices[read];
			bool moved = false;
			while (!moved && leavingCursor < leaving.size())
			{
				moved = TryReindexRealizedItemInPlace(
					leaving[leavingCursor++], target);
				auto* owner = dynamic_cast<ItemsControl*>(ownerLifetime.Get());
				if (!owner || owner->_itemsSource.Shared() != inPlaceSource
					|| owner->_generator.SourceCount() != inPlaceSourceCount
					|| owner->_generatedItemsRevision != inPlaceRevision)
					return false;
			}
			if (moved)
			{
				++reindexedInPlace;
				continue;
			}
			additionIndices[write++] = target;
		}
		additionIndices.resize(write);
	}
	std::vector<PreparedItem> additions;
	additions.reserve(additionIndices.size());
	const auto preparedSource = _itemsSource.Shared();
	const size_t preparedSourceCount = _generator.SourceCount();
	const size_t preparedGeneratedRevision = _generatedItemsRevision;
	bool additionsCommitted = false;
	ItemsScopeExit additionsGuard([
		ownerLifetime, &additions, &additionsCommitted,
		preparedSource, preparedSourceCount, preparedGeneratedRevision]
	{
		if (additionsCommitted) return;
		auto* owner = dynamic_cast<ItemsControl*>(ownerLifetime.Get());
		if (!owner || owner->_itemsSource.Shared() != preparedSource
			|| owner->_generator.SourceCount() != preparedSourceCount
			|| owner->_generatedItemsRevision != preparedGeneratedRevision) return;
		for (auto& prepared : additions)
		{
			if (!prepared.WasRecycled || !prepared.Visual
				|| owner->_generator.RecycledItems().contains(
					prepared.Index)) continue;
			try
			{
				owner->_generator.StoreRecycled(prepared.Index, {
					std::move(prepared.Visual),
					std::move(prepared.Observation) });
			}
			catch (...) {}
		}
	});
	for (const size_t index : additionIndices)
	{
		PreparedItem item;
		if (!PrepareGeneratedItem(
			index, item, true, additionIndices, recycleCandidates))
		{
			return false;
		}
		if (!ownerLifetime.Get()) return false;
		auto* owner = dynamic_cast<ItemsControl*>(ownerLifetime.Get());
		if (!owner || owner->_itemsSource.Shared() != preparedSource
			|| owner->_generator.SourceCount() != preparedSourceCount
			|| owner->_generatedItemsRevision != preparedGeneratedRevision)
			return false;
		additions.push_back(std::move(item));
	}

	auto& removals = _virtualRemovalIndicesScratch;
	removals.clear();
	removals.reserve(_generator.RealizedCount());
	for (const auto& [index, item] : _generator.RealizedItems())
	{
		(void)item;
		if (index < first || index >= last) removals.push_back(index);
	}
	if (additions.empty() && removals.empty() && reindexedInPlace == 0)
	{
		// PreparePresentation is evaluated for every retained frame.  A stable
		// virtual range is not a tree mutation and must not invalidate measure;
		// doing so creates a render -> realization -> layout -> render loop
		// (closed ComboBox instances are the common trigger).
		TrimRecyclePool(first, last);
		return true;
	}
	for (const auto index : removals)
	{
		auto* visual = _generator.GetRealized(index);
		if (visual)
		{
			if (auto* container = UnwrapGeneratedItem(visual))
				OnGeneratedItemClearing(*container);
			if (!ownerLifetime.Get()) return false;
			if (_generator.GetRealized(index) != visual) continue;
		}
		auto item = _generator.TakeRealized(index);
		visual = item.Visual;
		if (virtualHost)
			virtualHost->UnregisterItem(visual);
		auto detached = _itemsHost->DetachVisualChild(visual);
		if (detached)
		{
			(void)ClearGroupedItemLogicalParentPreservingOwnership(
				detached);
			std::exception_ptr parentError;
			if (detached)
				(void)cui::framework::TreeAccess::
					SetLogicalParentPreservingOwnership(
						detached, nullptr, &parentError);
			if (parentError)
				std::rethrow_exception(parentError);
		}
		if (detached)
			_generator.StoreRecycled(index, {
				std::move(detached), std::move(item.Observation) });
	}
	for (auto& addition : additions)
		AttachPreparedItem(std::move(addition));
	additionsCommitted = true;
	AdvanceGeneratedItemsRevision();
	TrimRecyclePool(first, last);
	_itemsHost->InvalidateLayout();
	if (localLayoutForScroll)
	{
		// The extent is unchanged, so a scroll-only realization can measure and
		// arrange its fixed host without scheduling a new root layout pass.
		_itemsHost->UpdateLayout();
		CommitItemsLayout();
	}
	else
		RequestLayout();
	OnGeneratedItemsRealized();
	return ownerLifetime.Get() != nullptr;
}

bool ItemsControl::TryReindexRealizedItemInPlace(
	size_t oldIndex, size_t newIndex)
{
	auto* virtualHost = dynamic_cast<VirtualizingItemsHost*>(_itemsHost);
	auto* visual = _generator.GetRealized(oldIndex);
	if (!virtualHost || !visual || oldIndex == newIndex
		|| _generator.ContainsRealized(newIndex)
		|| UnwrapGeneratedItem(visual) != visual
		|| visual->GetVisualParent() != _itemsHost
		|| !CanRebindRealizedItemInPlace(*visual, oldIndex)) return false;
	BindingSourceReference item;
	if (!_itemsSource || !_itemsSource.Get()->TryGetItem(newIndex, item)
		|| !item) return false;

	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference visualLifetime(visual);
	const ControlWeakReference hostLifetime(virtualHost);
	const auto source = _itemsSource.Shared();
	const size_t sourceCount = _generator.SourceCount();
	const size_t generatedRevision = _generatedItemsRevision;
	auto resolveOwner = [&]() -> ItemsControl*
	{
		auto* live = dynamic_cast<ItemsControl*>(ownerLifetime.Get());
		return live && live->_itemsSource.Shared() == source
			&& live->_generator.SourceCount() == sourceCount
			&& live->_generatedItemsRevision == generatedRevision
			&& live->_itemsHost == hostLifetime.Get()
			? live : nullptr;
	};
	// Same hook as the detach path: validation, details and UnloadingRow are
	// published while the container still represents its old item.
	OnGeneratedItemClearing(*visual);
	auto* live = resolveOwner();
	if (!live || visualLifetime.Get() != visual
		|| live->_generator.GetRealized(oldIndex) != visual
		|| live->_generator.ContainsRealized(newIndex)
		|| visual->GetVisualParent() != live->_itemsHost) return false;

	auto realized = live->_generator.TakeRealized(oldIndex);
	auto discard = [&]()
	{
		// A partially rebound container must never stay in the committed tree.
		auto* owner = dynamic_cast<ItemsControl*>(ownerLifetime.Get());
		auto* liveVisual = visualLifetime.Get();
		auto* host = dynamic_cast<VirtualizingItemsHost*>(hostLifetime.Get());
		if (!owner || !liveVisual || !host) return;
		host->UnregisterItem(liveVisual);
		if (liveVisual->GetVisualParent() != host) return;
		auto detached = host->DetachVisualChild(liveVisual);
		if (!detached) return;
		std::exception_ptr parentError;
		(void)cui::framework::TreeAccess::
			SetLogicalParentPreservingOwnership(
				detached, nullptr, &parentError);
	};
	std::wstring error;
	bool rebound = false;
	try
	{
		rebound = live->TryRebindRealizedItemInPlace(
			*visual, oldIndex, newIndex, item, realized.Observation, &error);
	}
	catch (...)
	{
		discard();
		throw;
	}
	live = resolveOwner();
	if (!live)
	{
		// The container is no longer tracked by the generator; a rebuild that
		// raced with the rebind would otherwise leave it orphaned in the host.
		discard();
		return false;
	}
	if (!rebound || visualLifetime.Get() != visual
		|| visual->GetVisualParent() != live->_itemsHost
		|| live->_generator.ContainsRealized(newIndex))
	{
		discard();
		return false;
	}
	auto* host = dynamic_cast<VirtualizingItemsHost*>(live->_itemsHost);
	if (!host)
	{
		discard();
		return false;
	}
	host->RegisterItem(visual, newIndex);
	live->_generator.StoreRealized(
		newIndex, visual, std::move(realized.Observation));
	return true;
}

bool ItemsControl::RealizeVirtualViewport(
	bool localLayoutForScroll, bool useVisibleOnlyRange)
{
	if (!IsVirtualizing()) return true;
	auto [first, last] = VirtualRangeForViewport();
	auto* scroll = ItemsScrollOwner();
	auto* host = dynamic_cast<VirtualizingItemsHost*>(_itemsHost);
	const auto& panel = EffectiveItemsPanel();
	const auto& realized = _generator.RealizedItems();
	const size_t count = ItemCount();
	if (useVisibleOnlyRange && scroll && host)
	{
		const auto size = scroll->GetActualSizeDip();
		const double viewport = std::isfinite(size.height)
			? (std::max)(1.0, static_cast<double>(size.height)) : 1.0;
		const double contentHeight = host->ContentHeight();
		const double offset = std::isfinite(scroll->VerticalOffset)
			? (std::clamp)(scroll->VerticalOffset, 0.0, contentHeight) : 0.0;
		const double visibleEndOffset = (std::min)(
			contentHeight, offset + viewport);
		first = host->IndexAtOffset(offset);
		last = visibleEndOffset >= contentHeight
			? count : (std::min)(count,
				host->IndexAtOffset(visibleEndOffset) + 1);
	}
	const bool canStabilize = scroll && host && !realized.empty()
		&& !useVisibleOnlyRange && !scroll->_draggingVerticalScrollBar
		&& panel.CacheLength > 0.0f;
	if (canStabilize)
	{
		const size_t currentFirst = realized.begin()->first;
		const size_t currentLast = realized.rbegin()->first + 1;
		const bool contiguous = currentLast <= count
			&& realized.size() == currentLast - currentFirst;
		const auto size = scroll->GetActualSizeDip();
		const double viewport = std::isfinite(size.height)
			? (std::max)(1.0, static_cast<double>(size.height)) : 1.0;
		const double contentHeight = host->ContentHeight();
		const double offset = std::isfinite(scroll->VerticalOffset)
			? (std::clamp)(scroll->VerticalOffset, 0.0, contentHeight) : 0.0;
		const double visibleEndOffset = (std::min)(
			contentHeight, offset + viewport);
		const size_t visibleFirst = host->IndexAtOffset(offset);
		const size_t visibleLast = visibleEndOffset >= contentHeight
			? count : (std::min)(count,
				host->IndexAtOffset(visibleEndOffset) + 1);
		if (contiguous && currentFirst <= visibleFirst
			&& currentLast >= visibleLast)
		{
			const double currentTop = currentFirst < count
				? host->ItemTop(currentFirst) : contentHeight;
			const double currentBottom = currentLast == 0
				? 0.0 : host->ItemTop(currentLast - 1)
					+ host->ItemExtent(currentLast - 1);
			const double guard = static_cast<double>(panel.CacheLength)
				* viewport * 0.35;
			const double delta = std::isfinite(_lastVirtualRealizationOffset)
				? offset - _lastVirtualRealizationOffset : 0.0;
			const bool forwardCovered = currentBottom - visibleEndOffset
				> guard && currentTop <= offset + 0.0001;
			const bool backwardCovered = offset - currentTop > guard
				&& currentBottom + 0.0001 >= visibleEndOffset;
			// Retire the trailing side at the exact cache boundary so stale
			// containers cannot accumulate.  Only the leading side gets hysteresis:
			// this batches expensive generation while keeping memory and UIA state
			// as precise as the pre-hysteresis viewport contract.
			if (delta > 0.0001 && forwardCovered)
				last = currentLast;
			else if (delta < -0.0001 && backwardCovered)
				first = currentFirst;
		}
	}
	const bool realizedRange = RealizeVirtualRange(
		first, last, localLayoutForScroll);
	if (realizedRange && scroll)
		_lastVirtualRealizationOffset = scroll->VerticalOffset;
	return realizedRange;
}

void ItemsControl::RestoreVirtualCacheAfterVerticalThumbDrag()
{
	if (!UseVisibleOnlyRangeDuringVerticalThumbDrag()) return;
	if (_realizingViewport || _applyingCollectionChange
		|| IsItemsSourceUpdateInProgress())
	{
		_virtualCacheRestorePending = true;
		RequestLayout();
		return;
	}
	_virtualCacheRestorePending = false;
	if (!RealizeVirtualViewport(true))
	{
		_virtualCacheRestorePending = true;
		RequestLayout();
	}
}

void ItemsControl::TrimRecyclePool(size_t first, size_t last)
{
	const size_t visible = (std::max)(size_t{ 1 }, last - first);
	const size_t limit = (std::max)(size_t{ 4 }, visible * 2);
	const size_t center = first + visible / 2;
	auto& recycled = _generator.RecycledItems();
	while (recycled.size() > limit)
	{
		const auto low = recycled.begin();
		const auto high = std::prev(recycled.end());
		const size_t lowDistance = center > low->first
			? center - low->first : low->first - center;
		const size_t highDistance = center > high->first
			? center - high->first : high->first - center;
		recycled.erase(highDistance >= lowDistance ? high : low);
	}
}

bool ItemsControl::TryBuildOccurrencePermutationReset(
	const CollectionChangedEventArgs& change,
	OccurrenceResetMapping& mapping)
{
	mapping = {};
	const ControlWeakReference ownerLifetime(this);
	const size_t count = ItemCount();
	if (!ownerLifetime.Get()) return false;
	const BindingListReference previousSource =
		_materializedItemsSourceSnapshot;
	const BindingListReference currentSource = _itemsSource;
	// Reset normally means that no index continuity may be assumed. DataGrid
	// sorting is the narrow exception: its active CollectionView emits one
	// WPF-style refresh while retaining a stable token for every physical
	// occurrence (including duplicate object references). Grouped rows carry
	// their headers in the same movable wrapper; GroupsChanged refreshes those
	// headers after the occurrence permutation commits.
	if (Type() != UIClass::UI_DataGrid
		|| change.Action != CollectionChangeAction::Reset
		|| change.OldSize != change.NewSize
		|| change.OldSize != count
		|| change.OldCount != change.OldSize
		|| change.NewCount != change.NewSize
		|| !_itemsHost || _applyingCollectionChange
		|| _generator.SourceCount() != count
		|| !currentSource || !previousSource)
		return false;
	if (!ownerLifetime.Get()) return false;
	const DataTypeToken currentType = currentSource.Get()->GetItemTypeToken();
	if (!ownerLifetime.Get()) return false;
	const DataTypeToken previousType = previousSource.Get()->GetItemTypeToken();
	if (!ownerLifetime.Get() || currentType != previousType) return false;
	const auto* previousIdentities =
		dynamic_cast<const IBindingListOccurrenceIdentity*>(
			previousSource.Get());
	const auto* currentIdentities =
		dynamic_cast<const IBindingListOccurrenceIdentity*>(
			currentSource.Get());
	if (!previousIdentities || !currentIdentities)
		return false;
	const size_t previousCount = previousSource.Get()->Count();
	if (!ownerLifetime.Get() || previousCount != count) return false;

	// A materialized CollectionView keeps an O(1) token-to-index table.  Prefer
	// that contract when it is available and remap only generated containers plus
	// the viewport anchor.  The legacy complete permutation below retains support
	// for identity-only third-party lists, but necessarily performs O(N) work.
	if (const auto* currentLookup =
		dynamic_cast<const IBindingListOccurrenceLookup*>(currentSource.Get());
		currentLookup
		&& currentLookup->IsItemIndexByOccurrenceIdentityLookupBounded())
	{
		std::unordered_set<size_t> occupied;
		occupied.reserve(
			_generator.RealizedCount() + _generator.RecycledCount());
		mapping.Generated.reserve(
			_generator.RealizedCount() + _generator.RecycledCount());
		const auto resolve = [&](size_t oldIndex, size_t& newIndex)
		{
			size_t token = 0;
			if (!previousIdentities->TryGetItemOccurrenceIdentity(
					oldIndex, token))
				return false;
			if (!ownerLifetime.Get()) return false;
			if (!currentLookup->TryGetItemIndexByOccurrenceIdentity(
					token, newIndex))
				return false;
			if (!ownerLifetime.Get()) return false;
			if (newIndex >= count) return false;

			BindingSourceReference previousItem;
			BindingSourceReference currentItem;
			if (!previousSource.Get()->TryGetItem(oldIndex, previousItem)
				|| !currentSource.Get()->TryGetItem(newIndex, currentItem))
				return false;
			if (!ownerLifetime.Get()) return false;
			if (!previousItem || !currentItem
				|| previousItem.Shared() != currentItem.Shared())
				return false;
			return true;
		};
		for (const auto& [oldIndex, item] : _generator.RealizedItems())
		{
			(void)item;
			size_t newIndex = 0;
			if (!resolve(oldIndex, newIndex)
				|| !occupied.insert(newIndex).second) return false;
			mapping.Generated.emplace_back(oldIndex, newIndex);
		}
		for (const auto& [oldIndex, item] : _generator.RecycledItems())
		{
			(void)item;
			size_t newIndex = 0;
			if (!resolve(oldIndex, newIndex)
				|| !occupied.insert(newIndex).second) return false;
			mapping.Generated.emplace_back(oldIndex, newIndex);
		}
		if (IsVirtualizing() && count != 0)
		{
			auto* virtualHost = dynamic_cast<VirtualizingItemsHost*>(_itemsHost);
			auto* scrollOwner = ItemsScrollOwner();
			if (!virtualHost) return false;
			const double offset = scrollOwner
				? scrollOwner->VerticalOffset : 0.0;
			const size_t oldAnchor = (std::min)(
				count - 1, virtualHost->IndexAtOffset(offset));
			size_t newAnchor = 0;
			if (!resolve(oldAnchor, newAnchor)) return false;
			mapping.ViewportAnchor = { oldAnchor, newAnchor };
		}
		mapping.Sparse = true;
		return true;
	}

	std::unordered_map<size_t, size_t> previousByToken;
	previousByToken.reserve(count);
	for (size_t oldIndex = 0; oldIndex < count; ++oldIndex)
	{
		size_t token = 0;
		if (!previousIdentities->TryGetItemOccurrenceIdentity(
			oldIndex, token)
			|| !previousByToken.emplace(token, oldIndex).second)
			return false;
		if (!ownerLifetime.Get()) return false;
	}

	std::vector<size_t> candidate(
		count, CollectionChangedEventArgs::Npos);
	for (size_t newIndex = 0; newIndex < count; ++newIndex)
	{
		size_t token = 0;
		if (!currentIdentities->TryGetItemOccurrenceIdentity(
			newIndex, token))
			return false;
		if (!ownerLifetime.Get()) return false;
		const auto found = previousByToken.find(token);
		if (found == previousByToken.end()
			|| candidate[found->second]
				!= CollectionChangedEventArgs::Npos)
			return false;

		// A token is a physical-occurrence identity, not permission to reuse a
		// container for a different record.  Verify the record identity as well
		// before committing any generator mutation.
		BindingSourceReference previousItem;
		BindingSourceReference currentItem;
		if (!previousSource.Get()->TryGetItem(
			found->second, previousItem)
			|| !currentSource.Get()->TryGetItem(newIndex, currentItem)
			|| !previousItem || !currentItem
			|| previousItem.Shared() != currentItem.Shared())
			return false;
		if (!ownerLifetime.Get()) return false;
		candidate[found->second] = newIndex;
	}
	if (std::any_of(candidate.begin(), candidate.end(), [](size_t index)
		{ return index == CollectionChangedEventArgs::Npos; })) return false;
	mapping.Complete = std::move(candidate);
	return true;
}

bool ItemsControl::ApplyOccurrencePermutationReset(
	const CollectionChangedEventArgs& change,
	const OccurrenceResetMapping& mapping)
{
	const ControlWeakReference ownerLifetime(this);
	const size_t newCount = ItemCount();
	if (!_itemsHost || _applyingCollectionChange
		|| change.Action != CollectionChangeAction::Reset
		|| (!mapping.Sparse && mapping.Complete.size() != newCount)
		|| _generator.SourceCount() != newCount)
		return false;
	const auto mappedIndex = [&](size_t oldIndex) -> std::optional<size_t>
	{
		if (!mapping.Sparse)
			return oldIndex < mapping.Complete.size()
				? std::optional<size_t>(mapping.Complete[oldIndex])
				: std::nullopt;
		const auto found = std::find_if(
			mapping.Generated.begin(), mapping.Generated.end(),
			[oldIndex](const auto& candidate)
			{ return candidate.first == oldIndex; });
		if (found != mapping.Generated.end()) return found->second;
		if (mapping.ViewportAnchor
			&& mapping.ViewportAnchor->first == oldIndex)
			return mapping.ViewportAnchor->second;
		return std::nullopt;
	};

	auto* scrollOwner = ItemsScrollOwner();
	double desiredScroll = scrollOwner
		? scrollOwner->VerticalOffset : 0.0;
	if (IsVirtualizing() && change.OldSize != 0)
	{
		auto* virtualHost = dynamic_cast<VirtualizingItemsHost*>(_itemsHost);
		if (!virtualHost) return false;
		const size_t oldAnchor = (std::min)(change.OldSize - 1,
			virtualHost->IndexAtOffset(desiredScroll));
		const double withinItem = desiredScroll
			- virtualHost->ItemTop(oldAnchor);
		ConfigureVirtualHost();
		const auto newAnchor = mappedIndex(oldAnchor);
		if (!newAnchor) return false;
		desiredScroll = virtualHost->ItemTop(*newAnchor)
			+ withinItem;
		const double contentHeight = virtualHost->ContentHeight();
		const double viewport = scrollOwner
			? (std::max)(1.0,
				static_cast<double>(scrollOwner->GetActualSizeDip().height))
			: contentHeight;
		desiredScroll = (std::clamp)(desiredScroll, 0.0,
			(std::max)(0.0, contentHeight - viewport));
	}
	else if (IsVirtualizing()) ConfigureVirtualHost();
	size_t first = 0;
	size_t last = newCount;
	if (IsVirtualizing())
		std::tie(first, last) = VirtualRangeForOffset(desiredScroll);
	std::unordered_set<size_t> occupied;
	occupied.reserve(_generator.RealizedCount());
	std::vector<size_t> removals;
	removals.reserve(_generator.RealizedCount());
	for (const auto& [oldIndex, item] : _generator.RealizedItems())
	{
		(void)item;
		const auto mapped = mappedIndex(oldIndex);
		if (!mapped) return false;
		const size_t newIndex = *mapped;
		occupied.insert(newIndex);
		if (IsVirtualizing()
			&& (newIndex < first || newIndex >= last))
			removals.push_back(newIndex);
	}
	std::vector<PreparedItem> additions;
	additions.reserve(last - first);
	OnBeforeGeneratedItemsPrepared();
	if (!ownerLifetime.Get()) return false;
	for (size_t index = first; index < last; ++index)
	{
		if (occupied.contains(index)) continue;
		PreparedItem item;
		// Recycled entries still use pre-permutation indices at this point.
		// Prepare only the viewport holes and do not consume the wrong entry.
		if (!PrepareGeneratedItem(index, item, false)) return false;
		if (!ownerLifetime.Get()) return false;
		additions.push_back(std::move(item));
	}
	_applyingCollectionChange = true;
	try
	{
		{
			// A pure permutation has already committed the generator and index
			// hooks before this notification returns.  Keep its invalidations in
			// the window's pending layout pass so the header click does not block
			// on a complete DataGrid measure/arrange traversal.
			ScopedLayoutUpdate layout(*this, false);
			OnBeforeGeneratedItemsRebuilt();
			if (!ownerLifetime.Get()) return false;
			const auto indexChanges = mapping.Sparse
				? _generator.ApplySparsePermutation(mapping.Generated)
				: _generator.ApplyPermutation(mapping.Complete);
			for (const auto& indexChange : indexChanges)
			{
				if (!indexChange.Visual) continue;
				// Only realized visuals belong to the virtual host. Recycled
				// containers are still re-indexed through the derived hook so
				// their ItemIndex is correct when they return to the viewport.
				if (auto* virtualHost =
					dynamic_cast<VirtualizingItemsHost*>(_itemsHost);
					virtualHost
					&& _generator.GetRealized(indexChange.NewIndex)
						== indexChange.Visual)
					virtualHost->RegisterItem(
						indexChange.Visual, indexChange.NewIndex);
				// The generator and virtual host track the outer grouped wrapper,
				// while derived controls own the logical item container inside it.
				// Publish index changes to that logical container just like the
				// prepare/clear hooks do; otherwise a grouped DataGridRow keeps its
				// old ItemIndex after a sort or an AddNew commit permutation.
				if (auto* container = UnwrapGeneratedItem(indexChange.Visual))
					OnGeneratedItemIndexChanged(
						*container,
						indexChange.OldIndex,
						indexChange.NewIndex);
				if (!ownerLifetime.Get()) return false;
			}

			for (const size_t index : removals)
			{
				auto* visual = _generator.GetRealized(index);
				if (visual)
				{
					if (auto* container = UnwrapGeneratedItem(visual))
						OnGeneratedItemClearing(*container);
					if (!ownerLifetime.Get()) return false;
					if (_generator.GetRealized(index) != visual) continue;
				}
				auto item = _generator.TakeRealized(index);
				if (!item.Visual) continue;
				if (auto* virtualHost =
					dynamic_cast<VirtualizingItemsHost*>(_itemsHost))
					virtualHost->UnregisterItem(item.Visual);
				auto detached = _itemsHost->DetachVisualChild(item.Visual);
				if (detached)
				{
					(void)ClearGroupedItemLogicalParentPreservingOwnership(
						detached);
					std::exception_ptr parentError;
					if (detached)
						(void)cui::framework::TreeAccess::
							SetLogicalParentPreservingOwnership(
								detached, nullptr, &parentError);
					if (parentError)
						std::rethrow_exception(parentError);
				}
				if (detached)
					_generator.StoreRecycled(index, {
						std::move(detached),
						std::move(item.Observation) });
			}
			for (auto& addition : additions)
			{
				_generator.DiscardRecycled(addition.Index);
				AttachPreparedItem(std::move(addition));
				if (!ownerLifetime.Get()) return false;
			}
			AdvanceGeneratedItemsRevision();
			ReorderRealizedChildren();
			ConfigureVirtualHost();
			_itemsHost->InvalidateLayout();
			RequestLayout();
			InvalidateVisual();
		}
		if (!ownerLifetime.Get()) return false;
		if (IsVirtualizing() && scrollOwner)
			scrollOwner->ScrollToVerticalOffset(desiredScroll);
		if (!ownerLifetime.Get()) return false;
		OnGeneratedItemsRebuilt();
		if (!ownerLifetime.Get()) return false;
		if (IsVirtualizing()) TrimRecyclePool(first, last);
		OnGeneratedItemsRealized();
		if (!ownerLifetime.Get()) return false;
		_applyingCollectionChange = false;
		return true;
	}
	catch (...)
	{
		if (auto* live = dynamic_cast<ItemsControl*>(ownerLifetime.Get()))
			live->_applyingCollectionChange = false;
		throw;
	}
}

bool ItemsControl::ApplyCollectionChange(
	const CollectionChangedEventArgs& change)
{
	const ControlWeakReference ownerLifetime(this);
	OccurrenceResetMapping occurrencePermutation;
	if (TryBuildOccurrencePermutationReset(
		change, occurrencePermutation))
		return ApplyOccurrencePermutationReset(
			change, occurrencePermutation);
	if (!ownerLifetime.Get()) return false;

	// Sorting preserves the item set and publishes a run of precise Move
	// notifications. Keep every generator/selection/current-item transition,
	// but let the final Move settle layout once for the complete reorder.
	// Size-changing actions retain their existing immediate-layout contract.
	const bool deferSynchronousLayout =
		change.Action == CollectionChangeAction::Move
		&& change.HasMoreChanges;
	const size_t newCount = ItemCount();
	if (!_itemsHost || _applyingCollectionChange
		|| !_generator.CanApply(change, newCount)) return false;

	auto* scrollOwner = ItemsScrollOwner();
	double desiredScroll = scrollOwner
		? scrollOwner->VerticalOffset : 0.0;
	if (IsVirtualizing() && change.OldSize != 0)
	{
		auto* virtualHost = dynamic_cast<VirtualizingItemsHost*>(_itemsHost);
		if (virtualHost)
		{
			const size_t oldAnchor = (std::min)(change.OldSize - 1,
				virtualHost->IndexAtOffset(desiredScroll));
			const double withinItem = desiredScroll
				- virtualHost->ItemTop(oldAnchor);
			auto mappedAnchor = ItemContainerGenerator::MapIndex(
				change, oldAnchor);
			if (!mappedAnchor
				&& change.Action == CollectionChangeAction::Replace
				&& change.NewCount != 0
				&& oldAnchor >= change.OldIndex
				&& oldAnchor - change.OldIndex < change.OldCount)
			{
				const size_t replacementOffset = (std::min)(
					oldAnchor - change.OldIndex, change.NewCount - 1);
				mappedAnchor = change.NewIndex + replacementOffset;
			}
			if (!mappedAnchor && newCount != 0)
			{
				const size_t replacement = change.Action
					== CollectionChangeAction::Replace
					? change.NewIndex : change.OldIndex;
				mappedAnchor = (std::min)(replacement, newCount - 1);
			}
			ConfigureVirtualHost();
			desiredScroll = mappedAnchor
				? virtualHost->ItemTop(*mappedAnchor) + withinItem : 0.0;
			const double contentHeight = virtualHost->ContentHeight();
			const double viewport = scrollOwner
				? (std::max)(1.0,
					static_cast<double>(scrollOwner->GetActualSizeDip().height))
				: contentHeight;
			desiredScroll = (std::clamp)(
				desiredScroll, 0.0,
				(std::max)(0.0, contentHeight - viewport));
		}
	}
	else if (IsVirtualizing()) ConfigureVirtualHost();

	size_t first = 0;
	size_t last = newCount;
	if (IsVirtualizing())
		std::tie(first, last) = VirtualRangeForOffset(desiredScroll);
	std::unordered_set<size_t> occupied;
	occupied.reserve(_generator.RealizedCount());
	for (const auto& [oldIndex, item] : _generator.RealizedItems())
	{
		(void)item;
		if (const auto mapped = ItemContainerGenerator::MapIndex(
			change, oldIndex)) occupied.insert(*mapped);
	}

	std::vector<PreparedItem> additions;
	additions.reserve(last - first);
	OnBeforeGeneratedItemsPrepared();
	if (!ownerLifetime.Get()) return false;
	for (size_t index = first; index < last; ++index)
	{
		if (occupied.contains(index)) continue;
		PreparedItem item;
		if (!PrepareGeneratedItem(index, item, false)) return false;
		if (!ownerLifetime.Get()) return false;
		additions.push_back(std::move(item));
	}

	_applyingCollectionChange = true;
	try
	{
		{
			ScopedLayoutUpdate layout(*this, !deferSynchronousLayout);
			OnBeforeGeneratedItemsRebuilt();
			if (!ownerLifetime.Get()) return false;
			for (const auto index
				: _generator.InvalidatedRealizedIndices(change))
			{
				auto* visual = _generator.GetRealized(index);
				if (visual)
				{
					if (auto* container = UnwrapGeneratedItem(visual))
						OnGeneratedItemClearing(*container);
					if (!ownerLifetime.Get()) return false;
					if (_generator.GetRealized(index) != visual) continue;
				}
				auto item = _generator.TakeRealized(index);
				if (!item.Visual) continue;
				if (auto* virtualHost =
					dynamic_cast<VirtualizingItemsHost*>(_itemsHost))
					virtualHost->UnregisterItem(item.Visual);
				auto detached = _itemsHost->DetachVisualChild(item.Visual);
				if (detached)
				{
					(void)ClearGroupedItemLogicalParentPreservingOwnership(
						detached);
					std::exception_ptr parentError;
					if (detached)
						(void)cui::framework::TreeAccess::
							SetLogicalParentPreservingOwnership(
								detached, nullptr, &parentError);
					if (parentError)
						std::rethrow_exception(parentError);
				}
			}
			for (const auto& indexChange : _generator.Apply(change))
			{
				if (!indexChange.Visual) continue;
				if (auto* virtualHost =
					dynamic_cast<VirtualizingItemsHost*>(_itemsHost))
					virtualHost->RegisterItem(
						indexChange.Visual, indexChange.NewIndex);
				if (auto* container = UnwrapGeneratedItem(indexChange.Visual))
					OnGeneratedItemIndexChanged(
						*container,
						indexChange.OldIndex,
						indexChange.NewIndex);
				if (!ownerLifetime.Get()) return false;
			}
			ConfigureVirtualHost();
			for (auto& addition : additions)
			{
				_generator.DiscardRecycled(addition.Index);
				AttachPreparedItem(std::move(addition));
				if (!ownerLifetime.Get()) return false;
			}
			AdvanceGeneratedItemsRevision();
			ReorderRealizedChildren();
			_itemsHost->InvalidateLayout();
			RequestLayout();
			InvalidateVisual();
		}
		if (!ownerLifetime.Get()) return false;
		if (!deferSynchronousLayout) UpdateLayout();
		if (!ownerLifetime.Get()) return false;
		if (IsVirtualizing() && scrollOwner)
			scrollOwner->ScrollToVerticalOffset(desiredScroll);
		if (!ownerLifetime.Get()) return false;
		OnGeneratedItemsRebuilt();
		if (!ownerLifetime.Get()) return false;
		if (IsVirtualizing())
		{
			(void)RealizeVirtualViewport();
			if (!ownerLifetime.Get()) return false;
			const auto range = VirtualRangeForViewport();
			TrimRecyclePool(range.first, range.second);
		}
		OnGeneratedItemsRealized();
		if (!ownerLifetime.Get()) return false;
		_applyingCollectionChange = false;
		return true;
	}
	catch (...)
	{
		if (auto* live = dynamic_cast<ItemsControl*>(ownerLifetime.Get()))
			live->_applyingCollectionChange = false;
		throw;
	}
}

void ItemsControl::RefreshGeneratedItem(
	const std::weak_ptr<IBindingSource>& itemIdentity)
{
	const auto item = itemIdentity.lock();
	if (!item || !_itemsSource) return;
	const int index = FindBindingListItemByValue(
		_itemsSource, CompiledBindingPathView{},
		BindingValue(BindingSourceReference(item)));
	if (index < 0) return;
	const size_t itemIndex = static_cast<size_t>(index);
	if (!_generator.ContainsRealized(itemIndex))
	{
		_generator.DiscardRecycled(itemIndex);
		return;
	}
	const CollectionChangedEventArgs refresh{
		CollectionChangeAction::Replace,
		itemIndex,
		itemIndex,
		1,
		1,
		ItemCount(),
		ItemCount()
	};
	(void)ApplyCollectionChange(refresh);
}

bool ItemsControl::RebuildGeneratedItems()
{
	_lastTemplateError.clear();
	const auto sourceItemType = _itemsSource
		? _itemsSource.Get()->GetItemTypeToken() : DataTypeToken{};
	if (_itemsSource && _itemTemplate
		&& !AreDataTypesCompatible(sourceItemType,
			_itemTemplate.Get()->GetDataTypeToken()))
	{
		_lastTemplateError = L"ItemTemplate DataType 与 ItemsSource ItemType 不一致。";
		return false;
	}
	if (_groupStyle)
	{
		const auto* style = _groupStyle.Get();
		if (style->HeaderTemplate
			&& !AreDataTypesCompatible(CollectionViewGroupDataTypeToken,
				style->HeaderTemplate.Get()->GetDataTypeToken()))
		{
			_lastTemplateError = L"GroupStyle HeaderTemplate DataType 必须为 CollectionViewGroup。";
			return false;
		}
	}
	if (!_itemsHost)
	{
		_lastTemplateError = L"ItemsHost 不可用。";
		return false;
	}
	RefreshVirtualGroupHeaderMetadata();

	_generator.ClearRecycled();
	std::vector<PreparedItem> prepared;
	size_t first = 0;
	size_t last = ItemCount();
	if (IsVirtualizing())
		std::tie(first, last) = VirtualRangeForViewport();
	prepared.reserve(last - first);
	OnBeforeGeneratedItemsPrepared();
	for (size_t index = first; index < last; ++index)
	{
		PreparedItem item;
		if (!PrepareGeneratedItem(index, item)) return false;
		prepared.push_back(std::move(item));
	}

	{
		ScopedLayoutUpdate layout(*this);
		OnBeforeGeneratedItemsRebuilt();
		ClearRealizedItems(false);
		_generator.SetSourceCount(ItemCount());
		ConfigureVirtualHost();
		for (auto& item : prepared) AttachPreparedItem(std::move(item));
		AdvanceGeneratedItemsRevision();
		OnGeneratedItemsRebuilt();
		_itemsHost->InvalidateLayout();
		RequestLayout();
		InvalidateVisual();
	}
	if (auto* scrollOwner = ItemsScrollOwner())
	{
		UpdateLayout();
		double verticalOffset = scrollOwner->VerticalOffset;
		if (auto* virtualHost =
			dynamic_cast<VirtualizingItemsHost*>(_itemsHost))
		{
			const double viewport = (std::max)(
				0.0, static_cast<double>(
					scrollOwner->GetActualSizeDip().height));
			verticalOffset = (std::min)(verticalOffset,
				std::ceil((std::max)(
					0.0, virtualHost->ContentHeight() - viewport)));
		}
		scrollOwner->ScrollToVerticalOffset(verticalOffset);
		if (IsVirtualizing()) (void)RealizeVirtualViewport();
	}
	return true;
}

bool ItemsControl::BringItemIntoView(size_t index)
{
	if (index >= ItemCount()) return false;
	auto* scrollOwner = ItemsScrollOwner();
	if (IsVirtualizing())
	{
		ConfigureVirtualHost();
		auto* host = dynamic_cast<VirtualizingItemsHost*>(_itemsHost);
		if (!host) return false;
		const double top = host->ItemTop(index);
		const double bottom = top + host->ItemExtent(index);
		if (!scrollOwner)
		{
			// DataGrid and popup-backed selectors deliberately defer virtual
			// realization until their template supplies a finite viewport.  A
			// programmatic CurrentItem/BringIntoView request can arrive while the
			// control is still on a hidden tab; realizing the complete source here
			// defeats that policy and turns one target lookup into N containers.
			// Keep the historical all-items behavior for hosts that opt into it,
			// but materialize only the requested occurrence for deferred hosts.
			const bool realizeWithoutViewport =
				ShouldRealizeVirtualItemsWithoutViewport();
			(void)RealizeVirtualRange(
				realizeWithoutViewport ? size_t{ 0 } : index,
				realizeWithoutViewport ? ItemCount() : index + 1);
			return GetGeneratedItem(index) != nullptr;
		}
		const double viewport = (std::max)(
			1.0, static_cast<double>(
				scrollOwner->GetActualSizeDip().height));
		double target = scrollOwner->VerticalOffset;
		if (top < target) target = top;
		else if (bottom > target + viewport)
			target = bottom - viewport;
		UpdateLayout();
		scrollOwner->ScrollToVerticalOffset(
			std::ceil((std::max)(0.0, target)));
		(void)RealizeVirtualViewport(true);
		if (auto* item = GetGeneratedItem(index))
		{
			(void)scrollOwner->BringDescendantIntoView(item);
			return true;
		}
		return false;
	}
	if (auto* item = GetGeneratedItem(index))
		return scrollOwner
			? scrollOwner->BringDescendantIntoView(item) : true;
	return false;
}

void ItemsControl::OnApplyTemplate()
{
	if (_pendingTemplateItemsPresenter.HasValue()
		&& !CommitPendingTemplateItemsPresenter())
		ClearPendingTemplateItemsPresenter();
	if (_templateItemsPresenter
		&& (!_controlTemplateRoot
			|| !VisualSubtreeContains(
				_controlTemplateRoot, _templateItemsPresenter)))
		throw std::logic_error(
			"ItemsControl active ItemsPresenter is outside the applied template root");
	Control::OnApplyTemplate();
}

void ItemsControl::PreparePresentation()
{
	if (_pendingTemplateItemsPresenter.HasValue())
		(void)CommitPendingTemplateItemsPresenter();
	Control::PreparePresentation();
	if (IsVirtualizing())
	{
		const bool restoring = _virtualCacheRestorePending
			&& !_realizingViewport && !_applyingCollectionChange
			&& !IsItemsSourceUpdateInProgress();
		const bool interactiveViewport = _itemsScrollOwner
			&& _itemsScrollOwner->_lastScrollChangeWasInteractive;
		const bool realized = RealizeVirtualViewport(
			_virtualRealizationPending || interactiveViewport,
			_virtualRealizationPending || interactiveViewport);
		if (realized) _virtualRealizationPending = false;
		if (restoring)
		{
			if (realized) _virtualCacheRestorePending = false;
			else RequestLayout();
		}
		else if (_virtualCacheRestorePending) RequestLayout();
	}
}

bool ItemsControl::ProcessInput(const InputReport& input)
{
	if (input.Kind == InputReportKind::FocusLost)
		ResetTextSearch();
	ProcessTextSearchKey(input);
	return Control::ProcessInput(input);
}

bool ItemsControl::ApplyTextInput(
	const TextCompositionEventArgs& input)
{
	if (!GetIsTextSearchEnabled() || input.Text.empty())
		return Control::ApplyTextInput(input);
	(void)ProcessTextSearchInput(input);
	// WPF marks committed text handled once text search is enabled even when
	// the current prefix has no matching item, preventing access-key fallback.
	return true;
}

void ItemsControl::OnRender()
{
	if (!IsVisible || !GetPresentationWindow() || !GetDrawingContext()) return;
	BeginRender();
	EndRender();
}

cui::core::Size ItemsControl::MeasureCore(
	const cui::core::Constraints& available)
{
	return cui::layout::MeasureOverlayChildren(
		GetLayoutChildrenView(), available,
		GetSpecifiedLayout().padding);
}

void ItemsControl::Arrange(cui::core::Rect finalRect)
{
	Control::Arrange(finalRect);
	PerformPendingLayout();
}

void ItemsControl::RequestLayout()
{
	_itemsLayoutPending = true;
	Control::RequestLayout();
}

void ItemsControl::OnComputedLayoutSizeChanged()
{
	_itemsLayoutPending = true;
}

void ItemsControl::PerformPendingLayout()
{
	if (IsLayoutSuspended() || !_itemsLayoutPending) return;
	if (GetControlTemplateRoot())
	{
		// Control::Arrange owns the ControlTemplate root slot. ItemsPanel
		// padding is expressed inside the template, not by shrinking its root.
		// Commit descendant changes without assigning the root a second slot.
		GetControlTemplateRoot()->UpdateLayout();
		_itemsLayoutPending = false;
		return;
	}
	const auto size = GetActualSizeDip();
	const auto padding = GetSpecifiedLayout().padding;
	cui::layout::ArrangeOverlayChildren(
		GetLayoutChildrenView(),
		cui::core::Rect{
			padding.left,
			padding.top,
			(std::max)(0.0f, size.width - padding.Horizontal()),
			(std::max)(0.0f, size.height - padding.Vertical()) });
	_itemsLayoutPending = false;
}

std::unique_ptr<Control> ItemsControl::WrapGeneratedItem(
	std::unique_ptr<Control> visual,
	const BindingSourceReference&,
	size_t)
{
	return visual;
}
