#include "DemoWindow.h"

#include <BindingList.h>
#include <Button.h>
#include <Canvas.h>
#include <ChartView.h>
#include <CheckBox.h>
#include <ComboBox.h>
#include <CollectionViewSource.h>
#include <ContentPresenter.h>
#include <Core/Threading.h>
#include <ContextMenu.h>
#include <DataGrid.h>
#include <Expander.h>
#include <EventInfrastructure.h>
#include <Label.h>
#include <ListView.h>
#include <ListBox.h>
#include <LoadingRing.h>
#include <ItemsPresenter.h>
#include <InputInfrastructure.h>
#include <MediaElement.h>
#include <Menu.h>
#include <MessageDialog.h>
#include <NativeSurface.h>
#include <NotifyIcon.h>
#include <NumericUpDown.h>
#include <Image.h>
#include <PresentationInfrastructure.h>
#include <StyleInfrastructure.h>
#include <WindowInfrastructure.h>
#include <PasswordBox.h>
#include <Popup.h>
#include <ProgressBar.h>
#include <ProgressRing.h>
#include <RadioButton.h>
#include <Layout/Grid.h>
#include <Layout/RelativePanel.h>
#include <Layout/StackPanel.h>
#include <RichTextBox.h>
#include <ScrollViewer.h>
#include <Slider.h>
#include <StatusBar.h>
#include <Style.h>
#include <Switch.h>
#include <ToggleButton.h>
#include <TabControl.h>
#include <TemplateInfrastructure.h>
#include <Taskbar.h>
#include <TextBox.h>
#include <ToolBar.h>
#include <TreeView.h>
#include <WebBrowser.h>
#include <Graphics.h>

#include <Utils.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace
{
	enum class DemoPage : int
	{
		Basic = 0,
		DateControls,
		Containers,
		Data,
		DataGrid,
		Analytics,
		Animation,
		Layout,
		System,
		WebBrowser,
		Media,
		WpfSemantics,
		TextComposition,
		Presentation
	};

	constexpr int PageIndex(DemoPage page) noexcept
	{
		return static_cast<int>(page);
	}

	constexpr size_t DemoDataGridExpectedColumnCount = 11;

	InputReport PointerInput(
		InputReportKind kind,
		MouseButton changedButton,
		int x,
		int y,
		MouseButton pressedButton = MouseButton::None)
	{
		InputReport input;
		input.Kind = kind;
		input.X = x;
		input.Y = y;
		input.ChangedButton = changedButton;
		input.ButtonStates = MouseButtonStates::WithPressed(pressedButton);
		input.ClickCount = kind == InputReportKind::PointerDoubleClick ? 2
			: kind == InputReportKind::PointerDown ? 1 : 0;
		return input;
	}

	InputReport KeyInput(InputReportKind kind, Key key)
	{
		InputReport input;
		input.Kind = kind;
		input.Key = key;
		return input;
	}

	InputReport LifecycleInput(InputReportKind kind)
	{
		InputReport input;
		input.Kind = kind;
		return input;
	}

	std::wstring CompositionStageText(TextCompositionStage stage)
	{
		switch (stage)
		{
		case TextCompositionStage::Started: return L"Started";
		case TextCompositionStage::Updated: return L"Updated";
		case TextCompositionStage::Completed: return L"Completed";
		case TextCompositionStage::Canceled: return L"Canceled";
		default: return L"Idle";
		}
	}

	std::wstring CompositionKindText(TextCompositionInputKind kind)
	{
		switch (kind)
		{
		case TextCompositionInputKind::Unicode: return L"Unicode";
		case TextCompositionInputKind::Ime: return L"IME";
		case TextCompositionInputKind::System: return L"System";
		case TextCompositionInputKind::Programmatic: return L"Programmatic";
		default: return L"Keyboard";
		}
	}

	std::wstring CompositionCancelText(TextCompositionCancelReason reason)
	{
		switch (reason)
		{
		case TextCompositionCancelReason::Explicit: return L"explicit";
		case TextCompositionCancelReason::FocusChanged: return L"focus-changed";
		case TextCompositionCancelReason::WindowDeactivated: return L"window-deactivated";
		case TextCompositionCancelReason::SourceDetached: return L"source-detached";
		case TextCompositionCancelReason::NativeCanceled: return L"native-canceled";
		case TextCompositionCancelReason::InvalidUnicode: return L"invalid-unicode";
		default: return L"none";
		}
	}

	std::wstring CompositionPayloadText(const TextCompositionEventArgs& e)
	{
		if (dynamic_cast<PasswordBox*>(e.OriginalSource))
			return L"<secure:" + std::to_wstring(e.Text.size())
				+ L" UTF-16 units>";
		return e.Text;
	}

	std::wstring FileNameFromPath(const std::wstring& path)
	{
		return std::filesystem::path(path).filename().wstring();
	}

	std::wstring ToJsStringLiteral(const std::wstring& value)
	{
		std::wstring result = L"\"";
		for (const auto ch : value)
		{
			switch (ch)
			{
			case L'\\': result += L"\\\\"; break;
			case L'\"': result += L"\\\""; break;
			case L'\r': result += L"\\r"; break;
			case L'\n': result += L"\\n"; break;
			default: result.push_back(ch); break;
			}
		}
		result += L"\"";
		return result;
	}

	std::wstring CommandParameterText(const std::any& parameter)
	{
		if (const auto* text = std::any_cast<std::wstring>(&parameter))
			return *text;
		if (const auto* text = std::any_cast<const wchar_t*>(&parameter))
			return *text ? std::wstring(*text) : std::wstring{};
		return {};
	}

	[[noreturn]] void ThrowRuntimeError(const std::wstring& message)
	{
		throw std::runtime_error(Convert::WStringToString(message));
	}

	class DemoUnpaidOrderTemplate final : public IItemTemplate
	{
	public:
		DataTypeToken GetDataTypeToken() const noexcept override
		{
			return MakeDataTypeToken(L"DemoOrder");
		}
#if CUI_ENABLE_DYNAMIC_XAML
		const std::wstring& DataTypeName() const noexcept override
		{
			static const std::wstring name = L"DemoOrder";
			return name;
		}
#endif
		std::unique_ptr<Control> Build(
			const BindingSourceReference&,
			size_t,
			std::wstring* outError) const override
		{
			if (outError) outError->clear();
			auto label = std::make_unique<Label>();
			label->Text = L"待付款";
			return label;
		}
	};

	class DemoPaidTemplateSelector final : public IItemTemplateSelector
	{
	public:
		DemoPaidTemplateSelector(
			ItemTemplateReference paid,
			ItemTemplateReference unpaid)
			: _paid(std::move(paid)), _unpaid(std::move(unpaid)) {}

		ItemTemplateReference SelectTemplate(
			const BindingSourceReference& item,
			Control&) const override
		{
			BindingValue value;
			bool paid = false;
			if (item && item.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Paid"), value))
				(void)value.TryGet(paid);
			return paid ? _paid : _unpaid;
		}

	private:
		ItemTemplateReference _paid;
		ItemTemplateReference _unpaid;
	};

	class DemoOrderRowStyleSelector final : public IItemStyleSelector
	{
	public:
		std::wstring SelectStyle(
			const BindingSourceReference& item,
			Control&) const override
		{
			BindingValue value;
			bool paid = false;
			if (item && item.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Paid"), value))
				(void)value.TryGet(paid);
			return paid ? L"OrderPaidRowStyle" : L"OrderGridRowStyle";
		}
	};

	class MillionOrderList final
		: public IBindingList,
		  public IBindingListOccurrenceIdentity,
		  public IBindingListOccurrenceLookup,
		  public IBindingListSnapshotProvider
	{
		struct MaterializedCache;
		struct SortPlan;
		class StableSnapshot;

	public:
		static constexpr size_t RowCount = 1'000'000;

		size_t Count() const noexcept override { return RowCount; }

		bool TryGetItem(
			size_t index, BindingSourceReference& out) const override
		{
			out = {};
			if (index >= RowCount) return false;
			return TryGetSourceItem(_sort.ViewToSource(index), out);
		}

		EventConnection SubscribeChanged(ChangedHandler handler) override
		{
			if (!handler) return {};
			return _changed.Subscribe(
				[handler = std::move(handler)](
					MillionOrderList*, const CollectionChangedEventArgs& change)
				{ handler(change); });
		}

		DataTypeToken GetItemTypeToken() const noexcept override
		{
			return MakeDataTypeToken(L"DemoOrder");
		}

#if CUI_ENABLE_DYNAMIC_XAML
		const std::wstring& ItemTypeName() const noexcept override
		{
			static const std::wstring name = L"DemoOrder";
			return name;
		}
#endif

		bool TryGetItemOccurrenceIdentity(
			size_t index, size_t& result) const noexcept override
		{
			result = 0;
			if (index >= RowCount) return false;
			const size_t sourceIndex = _sort.ViewToSource(index);
			if (sourceIndex == (std::numeric_limits<size_t>::max)())
				return false;
			result = sourceIndex + 1;
			return result != 0;
		}

		bool TryGetItemIndexByOccurrenceIdentity(
			size_t identity, size_t& index) const noexcept override
		{
			index = 0;
			if (identity == 0 || identity > RowCount) return false;
			index = _sort.SourceToView(identity - 1);
			return index < RowCount;
		}

		bool IsItemIndexByOccurrenceIdentityLookupBounded()
			const noexcept override { return true; }

		bool TryGetStableSnapshot(
			BindingListReference& result) const override
		{
			result = BindingListReference(std::make_shared<StableSnapshot>(
				_cache, _sort));
			return true;
		}

		bool ApplySort(
			size_t columnIndex, CollectionSortDirection direction)
		{
			SortPlan next;
			switch (columnIndex)
			{
			case 0:
			case 1:
				next.Reverse =
					direction == CollectionSortDirection::Descending;
				break;
			case 2:
				next = SortPlan::PeriodicStrings({
					L"华东", L"华南", L"华北", L"华中",
					L"西南", L"西北", L"东北" }, direction);
				break;
			case 3:
				next = SortPlan::PeriodicStrings({
					L"待确认", L"生产中", L"备货中",
					L"已发货", L"待付款", L"已完成" }, direction);
				break;
			case 4:
				next = SortPlan::PeriodicNumbers(48, direction);
				break;
			case 5:
				next = SortPlan::PeriodicNumbers(9'500, direction);
				break;
			case 6:
				next = SortPlan::PeriodicPaid(direction);
				break;
			default:
				return false;
			}
			ApplyModifiedSortOverrides(next, columnIndex, direction, _cache);
			_sort = std::move(next);
			const CollectionChangedEventArgs change{
				CollectionChangeAction::Reset,
				CollectionChangedEventArgs::Npos,
				CollectionChangedEventArgs::Npos,
				RowCount, RowCount, RowCount, RowCount };
			cui::framework::EventAccess::Raise(_changed, this, change);
			return true;
		}

		size_t MaterializedCount() const noexcept
		{
			return static_cast<size_t>(std::count_if(
				_cache->Items.begin(), _cache->Items.end(),
				[](const auto& item) { return !item.second.expired(); }));
		}

	private:
		struct MaterializedCache final
		{
			mutable std::unordered_map<
				size_t, std::weak_ptr<ObservableObject>> Items;
			// Generated rows remain weak so scrolling never becomes a million-object
			// cache.  A user edit promotes only that source occurrence to a strong,
			// sparse override which both snapshots and source-side sorting can see.
			mutable std::unordered_map<
				size_t, std::shared_ptr<ObservableObject>> ModifiedItems;
			mutable std::unordered_map<size_t, EventConnection> Observations;
		};

		struct SortPlan final
		{
			struct OverridePlacement final
			{
				size_t SourceIndex = 0;
				size_t InsertionRank = 0;
				size_t FinalPosition = 0;
			};

			bool Reverse = false;
			size_t Period = 0;
			std::vector<std::vector<size_t>> Buckets;
			std::vector<size_t> Starts;
			std::vector<size_t> ResidueBucket;
			std::vector<size_t> ResiduePosition;
			std::vector<size_t> RemovedBasePositions;
			std::vector<OverridePlacement> OverridesByFinal;
			std::vector<std::pair<size_t, size_t>> OverrideSourceToFinal;
			std::vector<size_t> OverrideInsertionRanks;

			size_t BaseViewToSource(size_t index) const noexcept
			{
				if (Period == 0) return Reverse ? RowCount - 1 - index : index;
				const auto upper = std::upper_bound(
					Starts.begin(), Starts.end(), index);
				if (upper == Starts.begin() || upper == Starts.end())
					return (std::numeric_limits<size_t>::max)();
				const size_t bucket = static_cast<size_t>(
					std::distance(Starts.begin(), upper) - 1);
				const size_t local = index - Starts[bucket];
				const auto& residues = Buckets[bucket];
				return (local / residues.size()) * Period
					+ residues[local % residues.size()];
			}

			size_t BaseSourceToView(size_t index) const noexcept
			{
				if (Period == 0) return Reverse ? RowCount - 1 - index : index;
				const size_t residue = index % Period;
				const size_t bucket = ResidueBucket[residue];
				return Starts[bucket] + (index / Period)
					* Buckets[bucket].size() + ResiduePosition[residue];
			}

			size_t ViewToSource(size_t index) const noexcept
			{
				if (index >= RowCount) return (std::numeric_limits<size_t>::max)();
				if (OverridesByFinal.empty()) return BaseViewToSource(index);
				const auto override = std::lower_bound(
					OverridesByFinal.begin(), OverridesByFinal.end(), index,
					[](const OverridePlacement& candidate, size_t position)
					{ return candidate.FinalPosition < position; });
				if (override != OverridesByFinal.end()
					&& override->FinalPosition == index)
					return override->SourceIndex;
				const size_t insertedBefore = static_cast<size_t>(std::distance(
					OverridesByFinal.begin(), override));
				const size_t nonOverrideRank = index - insertedBefore;
				size_t low = 0;
				size_t high = RowCount;
				while (low < high)
				{
					const size_t middle = low + (high - low) / 2;
					const size_t removedThrough = static_cast<size_t>(std::distance(
						RemovedBasePositions.begin(),
						std::upper_bound(RemovedBasePositions.begin(),
							RemovedBasePositions.end(), middle)));
					const size_t retainedThrough = middle + 1 - removedThrough;
					if (retainedThrough > nonOverrideRank) high = middle;
					else low = middle + 1;
				}
				return low < RowCount ? BaseViewToSource(low)
					: (std::numeric_limits<size_t>::max)();
			}

			size_t SourceToView(size_t index) const noexcept
			{
				if (index >= RowCount) return (std::numeric_limits<size_t>::max)();
				if (OverrideSourceToFinal.empty()) return BaseSourceToView(index);
				const auto override = std::lower_bound(
					OverrideSourceToFinal.begin(), OverrideSourceToFinal.end(), index,
					[](const auto& candidate, size_t sourceIndex)
					{ return candidate.first < sourceIndex; });
				if (override != OverrideSourceToFinal.end()
					&& override->first == index) return override->second;
				const size_t basePosition = BaseSourceToView(index);
				const size_t removedBefore = static_cast<size_t>(std::distance(
					RemovedBasePositions.begin(),
					std::lower_bound(RemovedBasePositions.begin(),
						RemovedBasePositions.end(), basePosition)));
				const size_t nonOverrideRank = basePosition - removedBefore;
				const size_t insertedBefore = static_cast<size_t>(std::distance(
					OverrideInsertionRanks.begin(),
					std::upper_bound(OverrideInsertionRanks.begin(),
						OverrideInsertionRanks.end(), nonOverrideRank)));
				return nonOverrideRank + insertedBefore;
			}

			static SortPlan PeriodicStrings(
				std::initializer_list<std::wstring_view> values,
				CollectionSortDirection direction)
			{
				std::vector<std::wstring_view> keys(values);
				return Build(keys.size(), direction,
					[&](size_t left, size_t right)
					{
						return keys[left] < keys[right] ? -1
							: keys[left] > keys[right] ? 1 : 0;
					});
			}

			static SortPlan PeriodicNumbers(
				size_t period, CollectionSortDirection direction)
			{
				return Build(period, direction,
					[](size_t left, size_t right)
					{
						return left < right ? -1 : left > right ? 1 : 0;
					});
			}

			static SortPlan PeriodicPaid(
				CollectionSortDirection direction)
			{
				return Build(3, direction,
					[](size_t left, size_t right)
					{
						const bool l = left != 0;
						const bool r = right != 0;
						return l < r ? -1 : l > r ? 1 : 0;
					});
			}

			template<typename Compare>
			static SortPlan Build(
				size_t period,
				CollectionSortDirection direction,
				Compare compare)
			{
				SortPlan result;
				result.Period = period;
				std::vector<size_t> residues(period);
				for (size_t index = 0; index < period; ++index)
					residues[index] = index;
				std::stable_sort(residues.begin(), residues.end(),
					[&](size_t left, size_t right)
					{
						const int value = compare(left, right);
						return direction == CollectionSortDirection::Ascending
							? value < 0 : value > 0;
					});
				for (const size_t residue : residues)
				{
					if (result.Buckets.empty()
						|| compare(result.Buckets.back().front(), residue) != 0)
						result.Buckets.push_back({});
					result.Buckets.back().push_back(residue);
				}
				for (auto& bucket : result.Buckets)
					std::sort(bucket.begin(), bucket.end());
				result.Starts.reserve(result.Buckets.size() + 1);
				result.Starts.push_back(0);
				result.ResidueBucket.resize(period);
				result.ResiduePosition.resize(period);
				for (size_t bucketIndex = 0;
					bucketIndex < result.Buckets.size(); ++bucketIndex)
				{
					size_t count = 0;
					for (size_t position = 0;
						position < result.Buckets[bucketIndex].size(); ++position)
					{
						const size_t residue =
							result.Buckets[bucketIndex][position];
						result.ResidueBucket[residue] = bucketIndex;
						result.ResiduePosition[residue] = position;
						if (residue < RowCount)
							count += (RowCount - 1 - residue) / period + 1;
					}
					result.Starts.push_back(result.Starts.back() + count);
				}
				return result;
			}
		};

		static BindingValue DefaultSortKey(
			size_t columnIndex, size_t sourceIndex)
		{
			static constexpr const wchar_t* regions[] =
			{
				L"华东", L"华南", L"华北", L"华中",
				L"西南", L"西北", L"东北"
			};
			static constexpr const wchar_t* stages[] =
			{
				L"待确认", L"生产中", L"备货中",
				L"已发货", L"待付款", L"已完成"
			};
			const auto number = static_cast<unsigned long long>(sourceIndex + 1);
			switch (columnIndex)
			{
			case 0:
				return BindingValue(StringHelper::Format(
					L"LOAD-%07llu", number));
			case 1:
				return BindingValue(StringHelper::Format(
					L"压力客户 %07llu", number));
			case 2:
				return BindingValue(std::wstring(
					regions[sourceIndex % std::size(regions)]));
			case 3:
				return BindingValue(std::wstring(
					stages[sourceIndex % std::size(stages)]));
			case 4:
				return BindingValue(static_cast<int>(sourceIndex % 48) + 1);
			case 5:
				return BindingValue(static_cast<long long>(
					8'000 + (sourceIndex % 9'500) * 37));
			case 6:
				return BindingValue(sourceIndex % 3 != 0);
			default:
				return {};
			}
		}

		static BindingSourcePropertyToken SortPropertyToken(size_t columnIndex)
		{
			static const BindingSourcePropertyToken tokens[] =
			{
				MakeBindingSourcePropertyToken(L"OrderNo"),
				MakeBindingSourcePropertyToken(L"Customer"),
				MakeBindingSourcePropertyToken(L"Region"),
				MakeBindingSourcePropertyToken(L"Stage"),
				MakeBindingSourcePropertyToken(L"Quantity"),
				MakeBindingSourcePropertyToken(L"Amount"),
				MakeBindingSourcePropertyToken(L"Paid")
			};
			return columnIndex < std::size(tokens)
				? tokens[columnIndex] : BindingSourcePropertyToken{};
		}

		static int CompareSortKeys(
			size_t columnIndex,
			const BindingValue& left,
			const BindingValue& right)
		{
			if (columnIndex <= 3)
			{
				const auto leftText = left.ToString();
				const auto rightText = right.ToString();
				const int compared = ::CompareStringOrdinal(
					leftText.c_str(), -1, rightText.c_str(), -1, TRUE);
				return compared == CSTR_LESS_THAN ? -1
					: compared == CSTR_GREATER_THAN ? 1 : 0;
			}
			if (columnIndex == 6)
			{
				bool leftValue = false;
				bool rightValue = false;
				(void)left.TryGet(leftValue);
				(void)right.TryGet(rightValue);
				return leftValue < rightValue ? -1
					: leftValue > rightValue ? 1 : 0;
			}
			long long leftValue = 0;
			long long rightValue = 0;
			(void)left.TryGet(leftValue);
			(void)right.TryGet(rightValue);
			return leftValue < rightValue ? -1
				: leftValue > rightValue ? 1 : 0;
		}

		static bool SortsBefore(
			size_t columnIndex,
			const BindingValue& leftKey, size_t leftSource,
			const BindingValue& rightKey, size_t rightSource,
			CollectionSortDirection direction)
		{
			const int compared = CompareSortKeys(columnIndex, leftKey, rightKey);
			if (compared == 0) return leftSource < rightSource;
			return direction == CollectionSortDirection::Ascending
				? compared < 0 : compared > 0;
		}

		static void ApplyModifiedSortOverrides(
			SortPlan& plan,
			size_t columnIndex,
			CollectionSortDirection direction,
			const std::shared_ptr<MaterializedCache>& cache)
		{
			if (!cache || cache->ModifiedItems.empty()) return;
			struct Candidate final
			{
				size_t SourceIndex = 0;
				BindingValue Key;
				size_t InsertionRank = 0;
				size_t FinalPosition = 0;
			};
			std::vector<Candidate> candidates;
			candidates.reserve(cache->ModifiedItems.size());
			const auto token = SortPropertyToken(columnIndex);
			for (const auto& [sourceIndex, item] : cache->ModifiedItems)
			{
				if (sourceIndex >= RowCount || !item) continue;
				BindingValue key;
				if (!token || !item->TryGetValue(token, key))
					key = DefaultSortKey(columnIndex, sourceIndex);
				candidates.push_back({ sourceIndex, std::move(key) });
			}
			if (candidates.empty()) return;

			plan.RemovedBasePositions.reserve(candidates.size());
			for (const auto& candidate : candidates)
				plan.RemovedBasePositions.push_back(
					plan.BaseSourceToView(candidate.SourceIndex));
			std::sort(plan.RemovedBasePositions.begin(),
				plan.RemovedBasePositions.end());
			std::sort(candidates.begin(), candidates.end(),
				[&](const Candidate& left, const Candidate& right)
				{
					return SortsBefore(columnIndex,
						left.Key, left.SourceIndex,
						right.Key, right.SourceIndex, direction);
				});
			for (size_t ordinal = 0; ordinal < candidates.size(); ++ordinal)
			{
				auto& candidate = candidates[ordinal];
				size_t low = 0;
				size_t high = RowCount;
				while (low < high)
				{
					const size_t middle = low + (high - low) / 2;
					const size_t baseSource = plan.BaseViewToSource(middle);
					const auto baseKey = DefaultSortKey(columnIndex, baseSource);
					if (SortsBefore(columnIndex,
						baseKey, baseSource,
						candidate.Key, candidate.SourceIndex, direction))
						low = middle + 1;
					else high = middle;
				}
				const size_t removedBefore = static_cast<size_t>(std::distance(
					plan.RemovedBasePositions.begin(),
					std::lower_bound(plan.RemovedBasePositions.begin(),
						plan.RemovedBasePositions.end(), low)));
				candidate.InsertionRank = low - removedBefore;
				candidate.FinalPosition = candidate.InsertionRank + ordinal;
			}

			plan.OverridesByFinal.reserve(candidates.size());
			plan.OverrideSourceToFinal.reserve(candidates.size());
			plan.OverrideInsertionRanks.reserve(candidates.size());
			for (const auto& candidate : candidates)
			{
				plan.OverridesByFinal.push_back({
					candidate.SourceIndex,
					candidate.InsertionRank,
					candidate.FinalPosition });
				plan.OverrideSourceToFinal.emplace_back(
					candidate.SourceIndex, candidate.FinalPosition);
				plan.OverrideInsertionRanks.push_back(
					candidate.InsertionRank);
			}
			std::sort(plan.OverrideSourceToFinal.begin(),
				plan.OverrideSourceToFinal.end());
		}

		class StableSnapshot final
			: public IBindingList,
			  public IBindingListOccurrenceIdentity,
			  public IBindingListOccurrenceLookup,
			  public IBindingListStableSnapshot
		{
		public:
			StableSnapshot(
				std::shared_ptr<MaterializedCache> cache, SortPlan sort)
				: _cache(std::move(cache)), _sort(std::move(sort)) {}
			size_t Count() const noexcept override { return RowCount; }
			bool TryGetItem(
				size_t index, BindingSourceReference& out) const override
			{
				return index < RowCount
					&& TryGetSourceItem(_cache, _sort.ViewToSource(index), out);
			}
			EventConnection SubscribeChanged(ChangedHandler) override { return {}; }
			DataTypeToken GetItemTypeToken() const noexcept override
			{
				return MakeDataTypeToken(L"DemoOrder");
			}
#if CUI_ENABLE_DYNAMIC_XAML
			const std::wstring& ItemTypeName() const noexcept override
			{
				static const std::wstring name = L"DemoOrder";
				return name;
			}
#endif
			bool TryGetItemOccurrenceIdentity(
				size_t index, size_t& result) const noexcept override
			{
				result = 0;
				if (index >= RowCount) return false;
				const size_t sourceIndex = _sort.ViewToSource(index);
				if (sourceIndex >= RowCount) return false;
				result = sourceIndex + 1;
				return true;
			}
			bool TryGetItemIndexByOccurrenceIdentity(
				size_t identity, size_t& index) const noexcept override
			{
				index = 0;
				if (identity == 0 || identity > RowCount) return false;
				index = _sort.SourceToView(identity - 1);
				return index < RowCount;
			}
			bool IsItemIndexByOccurrenceIdentityLookupBounded()
				const noexcept override { return true; }

		private:
			std::shared_ptr<MaterializedCache> _cache;
			SortPlan _sort;
		};

		bool TryGetSourceItem(
			size_t sourceIndex, BindingSourceReference& out) const
		{
			return TryGetSourceItem(_cache, sourceIndex, out);
		}

		static bool TryGetSourceItem(
			const std::shared_ptr<MaterializedCache>& cache,
			size_t sourceIndex,
			BindingSourceReference& out)
		{
			out = {};
			if (!cache || sourceIndex >= RowCount) return false;
			auto modified = cache->ModifiedItems.find(sourceIndex);
			auto item = modified == cache->ModifiedItems.end()
				? std::shared_ptr<ObservableObject>{} : modified->second;
			if (!item)
			{
				auto found = cache->Items.find(sourceIndex);
				item = found == cache->Items.end()
					? std::shared_ptr<ObservableObject>{}
					: found->second.lock();
			}
			if (!item)
			{
				item = CreateItem(sourceIndex);
				if (!item) return false;
				cache->Items[sourceIndex] = item;
				const std::weak_ptr<MaterializedCache> weakCache = cache;
				const std::weak_ptr<ObservableObject> weakItem = item;
				auto observation = item->PropertyChanged().Subscribe(
					[weakCache, weakItem, sourceIndex](
						const PropertyChangedEventArgs&)
					{
						auto liveCache = weakCache.lock();
						auto liveItem = weakItem.lock();
						if (!liveCache || !liveItem) return;
						const auto found = liveCache->Items.find(sourceIndex);
						if (found == liveCache->Items.end()
							|| found->second.lock() != liveItem) return;
						liveCache->ModifiedItems[sourceIndex] =
							std::move(liveItem);
					});
				cache->Observations.insert_or_assign(
					sourceIndex, std::move(observation));
				// Row containers and bindings retain every item that can still be
				// observed. Retire dead weak entries so a long manual scroll does not
				// turn this demo source into an accidental million-key cache.
				if (cache->Items.size() > 4096)
					for (auto candidate = cache->Items.begin();
						candidate != cache->Items.end();)
					{
						if (!candidate->second.expired())
						{
							++candidate;
							continue;
						}
						cache->Observations.erase(candidate->first);
						candidate = cache->Items.erase(candidate);
					}
			}
			out = BindingSourceReference(std::move(item));
			return true;
		}
		static std::shared_ptr<ObservableObject> CreateItem(size_t index)
		{
			static constexpr const wchar_t* regions[] =
			{
				L"华东", L"华南", L"华北", L"华中", L"西南", L"西北", L"东北"
			};
			static constexpr const wchar_t* stages[] =
			{
				L"待确认", L"生产中", L"备货中", L"已发货", L"待付款", L"已完成"
			};
			const auto number = static_cast<unsigned long long>(index + 1);
			auto item = std::make_shared<ObservableObject>();
			if (!item->DefineProperty(
				L"OrderNo", StringHelper::Format(L"LOAD-%07llu", number),
				true, false, true)
				|| !item->DefineProperty(
					L"DetailsUri", StringHelper::Format(
						L"https://example.test/orders/LOAD-%07llu", number),
					true, false, true)
				|| !item->DefineProperty(
					L"Customer", StringHelper::Format(
						L"压力客户 %07llu", number))
				|| !item->DefineProperty(L"Region", std::wstring(
					regions[index % std::size(regions)]))
				|| !item->DefineProperty(L"Stage", std::wstring(
					stages[index % std::size(stages)]))
				|| !item->DefineProperty(
					L"Quantity", static_cast<int>(index % 48) + 1)
				|| !item->DefineProperty(
					L"Amount", static_cast<long long>(
						8'000 + (index % 9'500) * 37))
				|| !item->DefineProperty(L"Paid", index % 3 != 0))
				return {};
			return item;
		}

		std::shared_ptr<MaterializedCache> _cache =
			std::make_shared<MaterializedCache>();
		SortPlan _sort;
		Event<void(MillionOrderList*, const CollectionChangedEventArgs&)> _changed;
	};

	class DemoSceneBehavior final : public INativeSurfaceBehavior
	{
	public:
		void Attach(NativeSurface&) override { _attached = true; }
		void Detach(NativeSurface&) noexcept override { _attached = false; }

		void Render(
			NativeSurface&,
			NativeSurfaceRenderContext& context) override
		{
			const auto width = std::max(1.0f, context.Bounds.width);
			const auto height = std::max(1.0f, context.Bounds.height);
			context.Graphics.FillRoundRect(
				0.0f, 0.0f, width, height,
				D2D1::ColorF(0.055f, 0.09f, 0.16f, 1.0f), 10.0f);
			for (int i = 1; i < 5; ++i)
			{
				const auto y = height * static_cast<float>(i) / 5.0f;
				context.Graphics.DrawLine(
					10.0f, y, width - 10.0f, y,
					D2D1::ColorF(0.18f, 0.31f, 0.48f, 0.65f), 1.0f);
			}
			const auto markerX = _hasPointer ? _pointerX : width * 0.66f;
			const auto markerY = _hasPointer ? _pointerY : height * 0.58f;
			context.Graphics.FillEllipse(
				markerX, markerY, 8.0f, 8.0f,
				D2D1::ColorF(0.12f, 0.68f, 0.54f, 1.0f));
			context.Graphics.DrawEllipse(
				markerX, markerY, 13.0f, 13.0f,
				D2D1::ColorF(0.25f, 0.82f, 1.0f, 0.9f), 2.0f);
			context.Graphics.DrawString(
				L"NativeSurface", 12.0f, 10.0f,
				D2D1::ColorF(0.88f, 0.94f, 1.0f, 1.0f));
			context.Graphics.DrawString(
				StringHelper::Format(L"C++ render · input %d", _inputCount),
				12.0f, 34.0f,
				D2D1::ColorF(0.52f, 0.72f, 0.94f, 1.0f));
			if (!_lastTextInput.empty())
				context.Graphics.DrawString(
					L"TextInput: " + _lastTextInput, 12.0f, 56.0f,
					D2D1::ColorF(0.72f, 0.90f, 0.82f, 1.0f));
		}

		bool HandleInput(
			NativeSurface& host,
			NativeSurfaceInputEvent& event) override
		{
			if (event.Kind == NativeSurfaceInputKind::TextInput)
			{
				_lastTextInput = event.Text;
				++_inputCount;
				host.InvalidateVisual();
				return true;
			}
			if (event.Kind != NativeSurfaceInputKind::PointerDown
				&& event.Kind != NativeSurfaceInputKind::PointerMove) return false;
			_pointerX = event.X;
			_pointerY = event.Y;
			_hasPointer = true;
			++_inputCount;
			host.InvalidateVisual();
			return true;
		}

		bool Attached() const noexcept { return _attached; }
		int InputCount() const noexcept { return _inputCount; }
		const std::wstring& LastTextInput() const noexcept
		{
			return _lastTextInput;
		}

	private:
		bool _attached = false;
		bool _hasPointer = false;
		float _pointerX = 0.0f;
		float _pointerY = 0.0f;
		int _inputCount = 0;
		std::wstring _lastTextInput;
	};

	class PresentationProbeBehavior final : public INativeSurfaceBehavior
	{
	public:
		void Attach(NativeSurface&) override { _attached = true; }
		void Detach(NativeSurface&) noexcept override { _attached = false; }

		void Render(
			NativeSurface& host,
			NativeSurfaceRenderContext& context) override
		{
			++_frameCount;
			_lastDpiScale = context.DpiScale;
			const float width = std::max(1.0f, context.Bounds.width);
			const float height = std::max(1.0f, context.Bounds.height);
			auto& graphics = context.Graphics;
			graphics.FillRoundRect(0.0f, 0.0f, width, height,
				D2D1::ColorF(0.035f, 0.055f, 0.10f, 1.0f), 12.0f);
			for (int column = 1; column < 8; ++column)
			{
				const float x = width * static_cast<float>(column) / 8.0f;
				graphics.DrawLine(x, 0.0f, x, height,
					D2D1::ColorF(0.12f, 0.22f, 0.36f, 0.55f), 1.0f);
			}
			for (int row = 1; row < 5; ++row)
			{
				const float y = height * static_cast<float>(row) / 5.0f;
				graphics.DrawLine(0.0f, y, width, y,
					D2D1::ColorF(0.12f, 0.22f, 0.36f, 0.55f), 1.0f);
			}
			const float markerX = _hasMarker ? _markerX : width * 0.70f;
			const float markerY = _hasMarker ? _markerY : height * 0.62f;
			graphics.FillEllipse(markerX, markerY, 10.0f, 10.0f,
				D2D1::ColorF(0.06f, 0.78f, 0.58f, 1.0f));
			graphics.DrawEllipse(markerX, markerY, 17.0f, 17.0f,
				D2D1::ColorF(0.24f, 0.72f, 1.0f, 0.95f), 2.0f);
			graphics.DrawString(L"PresentationRenderHost frame",
				16.0f, 14.0f, D2D1::ColorF(0.88f, 0.94f, 1.0f, 1.0f));
			graphics.DrawString(StringHelper::Format(
				L"presented %d · region requests %d · full requests %d",
				_frameCount, _regionRequests, _fullFrameRequests),
				16.0f, 42.0f, D2D1::ColorF(0.48f, 0.76f, 0.96f, 1.0f));
			if (host.GetPresentationWindow())
			{
				const auto stats =
					cui::framework::WindowAccess::PresentationFrame(*host.GetPresentationWindow());
				graphics.DrawString(StringHelper::Format(
					L"DPI %.2fx · resource gen %llu · behavior resets %d",
					_lastDpiScale,
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationResourceGeneration(*host.GetPresentationWindow())),
					_deviceGeneration),
					16.0f, 68.0f,
					D2D1::ColorF(0.52f, 0.86f, 0.68f, 1.0f));
				graphics.DrawString(StringHelper::Format(
					L"scene r%llu · nodes %llu · drawing segments %llu",
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationSceneRevision(*host.GetPresentationWindow())),
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationNodeCount(*host.GetPresentationWindow())),
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationDrawingLayerCount(*host.GetPresentationWindow()))),
					16.0f, 94.0f,
					D2D1::ColorF(0.92f, 0.72f, 0.28f, 1.0f));
				graphics.DrawString(StringHelper::Format(
					L"lanes · content %llu · geometry %llu · composition %llu",
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationContentRevision(*host.GetPresentationWindow())),
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationGeometryRevision(*host.GetPresentationWindow())),
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationCompositionRevision(*host.GetPresentationWindow()))),
					16.0f, 120.0f,
					D2D1::ColorF(0.72f, 0.82f, 0.98f, 1.0f));
				graphics.DrawString(StringHelper::Format(
					L"frame %llu · dirty C/G/P %llu/%llu/%llu · geom %llu · replay %llu",
					static_cast<unsigned long long>(stats.Frame),
					static_cast<unsigned long long>(stats.ContentDirtyNodes),
					static_cast<unsigned long long>(stats.GeometryDirtyNodes),
					static_cast<unsigned long long>(stats.CompositionDirtyNodes),
					static_cast<unsigned long long>(stats.GeometryRecomputedNodes),
					static_cast<unsigned long long>(stats.DamageReplayNodes)),
					16.0f, 146.0f,
					D2D1::ColorF(0.62f, 0.90f, 0.70f, 1.0f));
				graphics.DrawString(StringHelper::Format(
					L"commands · record %llu · replay %llu · hits %llu · moved %llu · kept %llu · invalidated %llu",
					static_cast<unsigned long long>(stats.CommandRecordedNodes),
					static_cast<unsigned long long>(stats.CommandReplayedNodes),
					static_cast<unsigned long long>(stats.CommandCacheHitNodes),
					static_cast<unsigned long long>(stats.CommandTranslatedNodes),
					static_cast<unsigned long long>(
						stats.CommandCacheRetainedNodes),
					static_cast<unsigned long long>(
						stats.CommandCacheInvalidatedNodes)),
					16.0f, 172.0f,
					D2D1::ColorF(0.96f, 0.74f, 0.38f, 1.0f));
				graphics.DrawString(StringHelper::Format(
					L"transaction %llu · committed %llu · aborted %llu · recovered %llu",
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationTransactionSequence(*host.GetPresentationWindow())),
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationCommittedFrameCount(*host.GetPresentationWindow())),
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationAbortedFrameCount(*host.GetPresentationWindow())),
					static_cast<unsigned long long>(
						cui::framework::WindowAccess::PresentationDeviceRecoveryCount(*host.GetPresentationWindow()))),
					16.0f, 198.0f,
					D2D1::ColorF(0.72f, 0.82f, 0.98f, 1.0f));
			}
		}

		bool HandleInput(
			NativeSurface& host,
			NativeSurfaceInputEvent& event) override
		{
			if (event.Kind != NativeSurfaceInputKind::PointerDown
				&& event.Kind != NativeSurfaceInputKind::PointerMove) return false;
			MoveMarker(host, event.X, event.Y);
			return true;
		}

		void DpiChanged(NativeSurface&, float dpiScale) override
		{
			_lastDpiScale = dpiScale;
		}

		void DeviceResourcesInvalidated(NativeSurface&) noexcept override
		{
			++_deviceGeneration;
		}

		void Pulse(NativeSurface& host)
		{
			const auto size = host.GetActualSizeDip();
			const float width = std::max(80.0f, size.width);
			const float height = std::max(80.0f, size.height);
			const float x = 40.0f + std::fmod(
				static_cast<float>((_regionRequests + 1) * 97),
				std::max(1.0f, width - 80.0f));
			const float y = 40.0f + std::fmod(
				static_cast<float>((_regionRequests + 1) * 53),
				std::max(1.0f, height - 80.0f));
			MoveMarker(host, x, y);
		}

		void NoteFullFrameRequest() noexcept { ++_fullFrameRequests; }
		bool Attached() const noexcept { return _attached; }
		int RegionRequests() const noexcept { return _regionRequests; }
		int FullFrameRequests() const noexcept { return _fullFrameRequests; }
		int FrameCount() const noexcept { return _frameCount; }
		int DeviceResourceInvalidations() const noexcept
		{
			return _deviceGeneration;
		}

	private:
		void MoveMarker(NativeSurface& host, float x, float y)
		{
			const float oldX = _hasMarker ? _markerX : x;
			const float oldY = _hasMarker ? _markerY : y;
			_markerX = x;
			_markerY = y;
			_hasMarker = true;
			++_regionRequests;
			host.InvalidateRegion(D2D1_RECT_F{
				std::min(oldX, x) - 22.0f,
				std::min(oldY, y) - 22.0f,
				std::max(oldX, x) + 22.0f,
				std::max(oldY, y) + 22.0f });
		}

		bool _attached = false;
		bool _hasMarker = false;
		float _markerX = 0.0f;
		float _markerY = 0.0f;
		float _lastDpiScale = 1.0f;
		int _frameCount = 0;
		int _regionRequests = 0;
		int _fullFrameRequests = 0;
		int _deviceGeneration = 0;
	};
}

template<typename T>
T* DemoWindow::RequireControl(const wchar_t* name)
{
	auto* control = dynamic_cast<T*>(FindGeneratedControlByName(name));
	if (!control) ThrowRuntimeError(L"XAML 缺少控件或类型不匹配：" + std::wstring(name));
	return control;
}

Control* DemoWindow::FindGeneratedControlByName(
	std::wstring_view name) const noexcept
{
	// x:Name is compiled into strongly typed fields; it is not retained as a
	// runtime string dictionary in production.
	if (name == L"analyticsQuery") return analyticsQuery;
	if (name == L"analyticsReset") return analyticsReset;
	if (name == L"analyticsRows") return analyticsRows;
	if (name == L"animationGallery") return animationGallery;
	if (name == L"authoredStateTree") return authoredStateTree;
	if (name == L"basicButton") return basicButton;
	if (name == L"basicCombo") return basicCombo;
	if (name == L"canvasLeftWins") return canvasLeftWins;
	if (name == L"canvasRightBottom") return canvasRightBottom;
	if (name == L"canvasSemanticsProbe") return canvasSemanticsProbe;
	if (name == L"chartBar") return chartBar;
	if (name == L"chartPie") return chartPie;
	if (name == L"commandTargetButton") return commandTargetButton;
	if (name == L"commandTargetTrace") return commandTargetTrace;
	if (name == L"composedDensityEditor") return composedDensityEditor;
	if (name == L"compositionCancelProbe") return compositionCancelProbe;
	if (name == L"compositionCommitProbe") return compositionCommitProbe;
	if (name == L"compositionFocusProbe") return compositionFocusProbe;
	if (name == L"compositionPasswordBox") return compositionPasswordBox;
	if (name == L"compositionPreviewHandledProbe")
		return compositionPreviewHandledProbe;
	if (name == L"compositionResetProbe") return compositionResetProbe;
	if (name == L"compositionRichTextBox") return compositionRichTextBox;
	if (name == L"compositionStartProbe") return compositionStartProbe;
	if (name == L"compositionState") return compositionState;
	if (name == L"compositionStats") return compositionStats;
	if (name == L"compositionSurrogateProbe") return compositionSurrogateProbe;
	if (name == L"compositionTextBox") return compositionTextBox;
	if (name == L"compositionTrace") return compositionTrace;
	if (name == L"compositionUnicharProbe") return compositionUnicharProbe;
	if (name == L"compositionUpdateProbe") return compositionUpdateProbe;
	if (name == L"dataGridCurrentColumnState") return dataGridCurrentColumnState;
	if (name == L"dataGridCurrentItemState") return dataGridCurrentItemState;
	if (name == L"dataGridStatus") return dataGridStatus;
	if (name == L"dataGridMillionButton") return dataGridMillionButton;
	if (name == L"dataGridSurface") return dataGridSurface;
	if (name == L"demoDataGrid") return demoDataGrid;
	if (name == L"demoImage") return demoImage;
	if (name == L"demoList") return demoList;
	if (name == L"demoListBox") return demoListBox;
	if (name == L"demoProgress") return demoProgress;
	if (name == L"demoRelative") return demoRelative;
	if (name == L"demoScene") return demoScene;
	if (name == L"demoScroll") return demoScroll;
	if (name == L"demoTree") return demoTree;
	if (name == L"dialogCancelButton") return dialogCancelButton;
	if (name == L"dismissToast") return dismissToast;
	if (name == L"enableInput") return enableInput;
	if (name == L"farButton") return farButton;
	if (name == L"featureActionA") return featureActionA;
	if (name == L"featureActionB") return featureActionB;
	if (name == L"featureCard") return featureCard;
	if (name == L"featureCardContent") return featureCardContent;
	if (name == L"globalProgress") return globalProgress;
	if (name == L"indeterminateProgress") return indeterminateProgress;
	if (name == L"layoutSurface") return layoutSurface;
	if (name == L"loadingRing") return loadingRing;
	if (name == L"mainMenu") return mainMenu;
	if (name == L"mainStatusBar") return mainStatusBar;
	if (name == L"mainTabs") return mainTabs;
	if (name == L"mainToolBar") return mainToolBar;
	if (name == L"mediaOpen") return mediaOpen;
	if (name == L"mediaPause") return mediaPause;
	if (name == L"mediaPlay") return mediaPlay;
	if (name == L"mediaElement") return mediaElement;
	if (name == L"mediaProgress") return mediaProgress;
	if (name == L"mediaSpeedText") return mediaSpeedText;
	if (name == L"mediaTime") return mediaTime;
	if (name == L"nameInput") return nameInput;
	if (name == L"naturalTextProbe") return naturalTextProbe;
	if (name == L"notifyBalloon") return notifyBalloon;
	if (name == L"notifyToggle") return notifyToggle;
	if (name == L"presentationProbeSurface") return presentationProbeSurface;
	if (name == L"presentationStatus") return presentationStatus;
	if (name == L"presentationTopologyTile") return presentationTopologyTile;
	if (name == L"progressRing") return progressRing;
	if (name == L"radioA") return radioA;
	if (name == L"radioB") return radioB;
	if (name == L"relativeCenter") return relativeCenter;
	if (name == L"relativeCenterButton") return relativeCenterButton;
	if (name == L"salesChart") return salesChart;
	if (name == L"showDialog") return showDialog;
	if (name == L"sideNavigationList") return sideNavigationList;
	if (name == L"splitNotes") return splitNotes;
	if (name == L"statusText") return statusText;
	if (name == L"systemContextMenu") return systemContextMenu;
	if (name == L"systemSurface") return systemSurface;
	if (name == L"textCompositionLabSurface") return textCompositionLabSurface;
	if (name == L"themeDisabledButton") return themeDisabledButton;
	if (name == L"themeNormalButton") return themeNormalButton;
	if (name == L"toastMessage") return toastMessage;
	if (name == L"toolAnalytics") return toolAnalytics;
	if (name == L"toolAnimation") return toolAnimation;
	if (name == L"toolBasic") return toolBasic;
	if (name == L"toolData") return toolData;
	if (name == L"toolIcon1") return toolIcon1;
	if (name == L"toolIcon2") return toolIcon2;
	if (name == L"toolIcon3") return toolIcon3;
	if (name == L"toolIconImage1") return toolIconImage1;
	if (name == L"toolIconImage2") return toolIconImage2;
	if (name == L"toolIconImage3") return toolIconImage3;
	if (name == L"toolSeparator") return toolSeparator;
	if (name == L"toolSystem") return toolSystem;
	if (name == L"trimmedTextProbe") return trimmedTextProbe;
	if (name == L"verticalThemeProgress") return verticalThemeProgress;
	if (name == L"verticalThemeSlider") return verticalThemeSlider;
	if (name == L"webBrowser") return webBrowser;
	if (name == L"windowContent") return windowContent;
	if (name == L"wpfAncestorValue") return wpfAncestorValue;
	if (name == L"wpfBindingScope") return wpfBindingScope;
	if (name == L"wpfConvertedValue") return wpfConvertedValue;
	if (name == L"wpfDispatcherProbe") return wpfDispatcherProbe;
	if (name == L"wpfDispatcherResult") return wpfDispatcherResult;
	if (name == L"wpfElementMirror") return wpfElementMirror;
	if (name == L"wpfFallbackValue") return wpfFallbackValue;
	if (name == L"wpfFocusPeerB") return wpfFocusPeerB;
	if (name == L"wpfFocusPeerC") return wpfFocusPeerC;
	if (name == L"wpfHierarchyScope") return wpfHierarchyScope;
	if (name == L"wpfIndexerValue") return wpfIndexerValue;
	if (name == L"wpfInnerResourceValue") return wpfInnerResourceValue;
	if (name == L"wpfInputStats") return wpfInputStats;
	if (name == L"wpfKeyedIndexerValue") return wpfKeyedIndexerValue;
	if (name == L"wpfLabSurface") return wpfLabSurface;
	if (name == L"wpfMultiValue") return wpfMultiValue;
	if (name == L"wpfNoFocusPeer") return wpfNoFocusPeer;
	if (name == L"wpfNullValue") return wpfNullValue;
	if (name == L"wpfRouteMiddle") return wpfRouteMiddle;
	if (name == L"wpfRouteOuter") return wpfRouteOuter;
	if (name == L"wpfRouteSource") return wpfRouteSource;
	if (name == L"wpfRouteTrace") return wpfRouteTrace;
	if (name == L"wpfScopeResourceValue") return wpfScopeResourceValue;
	if (name == L"wpfSelfValue") return wpfSelfValue;
	if (name == L"wpfTemplateButton") return wpfTemplateButton;
	if (name == L"wpfTemplateList") return wpfTemplateList;
	if (name == L"wpfTextInputSource") return wpfTextInputSource;
	if (name == L"wpfTriggerButton") return wpfTriggerButton;
	if (name == L"wpfTwoWayEditor") return wpfTwoWayEditor;
	if (name == L"wpfTypographyOverride") return wpfTypographyOverride;
	if (name == L"wrappedTextProbe") return wrappedTextProbe;
	return nullptr;
}

DemoWindow::DemoWindow(InitializationMode mode)
	: DemoWindowGenerated()
{
	PrepareRuntimeData();
	InitializeComponent();
	if (!BindData(BindingSourceReference(_dataContext)))
		ThrowRuntimeError(L"AOT 生成树无法连接运行时 DataContext。");
	ResolveControls();
	AttachStaticBehaviors();
	RegisterClassCommandBindings();
	if (mode == InitializationMode::DeclarativeOnly) return;
	InitializeBasicPage();
	InitializeContainerPage();
	InitializeDataPage();
	InitializeDataGridPage();
	InitializeAnalyticsPage();
	InitializeWebPage();
	InitializeMediaPage();
	_runtimeDataInitialized = true;
	if (mode == InitializationMode::Full) InitializeSystemPage();
	UpdateProgress(0.25f);
}

DemoWindow::~DemoWindow()
{
	if (_notify) (void)_notify->TryHide();
}

bool DemoWindow::VerifyDeclarativeFeatures(std::wstring* outError)
{
	auto fail = [&](std::wstring message)
	{
		if (outError) *outError = std::move(message);
		return false;
	};
	try
	{
		if (!_componentInitialized || !windowContent || !runtimeBadge
			|| runtimeBadge->GetDisplayText() != L"AOT XAML · Native C++")
			return fail(L"AOT InitializeComponent 未构造完整的生成控件树。");
		const auto& labelClip = gradientLabel
			? gradientLabel->GetClip()
			: std::optional<cui::drawing::Geometry>{};
		const auto& labelTransform = gradientLabel
			? gradientLabel->GetRenderTransform()
			: std::optional<cui::drawing::Transform>{};
		if (!gradientLabel
			|| gradientLabel->Padding
				!= Thickness(12.0f, 0.0f, 12.0f, 0.0f)
			|| gradientLabel->GetPropertyValueSource(
				Control::PaddingProperty())
				!= DependencyPropertyValueSource::Local
			|| !labelClip || labelClip->ContainsPoint(D2D1::Point2F())
			|| !labelClip->ContainsPoint(D2D1::Point2F(12.0f, 17.0f))
			|| !labelTransform || labelTransform->Operations.size() != 2
			|| labelTransform->Operations[0].Kind
				!= cui::drawing::TransformKind::Rotate
			|| labelTransform->Operations[0].Angle != -2.0f)
			return fail(L"gradientLabel 的 Padding/Clip/RenderTransform "
				L"未保持 WPF 局部坐标语义。");
		auto applyTemplateForVerification =
			[&](Control* current, std::wstring_view label)
		{
			if (!current || !current->GetTemplate()) return true;
			(void)current->ApplyTemplate();
			return current->LastTemplateError().empty()
				|| fail(L"生成的 ControlTemplate 应用失败（"
					+ std::wstring(label) + L"）："
					+ current->LastTemplateError());
		};
		auto* themeNormalButton = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"themeNormalButton"));
		auto* themeDisabledButton = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"themeDisabledButton"));
		if (!applyTemplateForVerification(_basicButton, L"basicButton")
			|| !applyTemplateForVerification(
				themeNormalButton, L"themeNormalButton")
			|| !applyTemplateForVerification(
				themeDisabledButton, L"themeDisabledButton"))
			return false;
		auto* basicChrome = _basicButton
			? _basicButton->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_Chrome"))
			: nullptr;
		auto* basicPresenter = _basicButton
			? dynamic_cast<ContentPresenter*>(
				_basicButton->FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"PART_ContentPresenter")))
			: nullptr;
		auto* basicRoot = _basicButton
			? cui::framework::TemplateAccess::GetTemplateRoot(*_basicButton)
			: nullptr;
		auto* normalChrome = themeNormalButton
			? themeNormalButton->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_Chrome"))
			: nullptr;
		auto* disabledChrome = themeDisabledButton
			? themeDisabledButton->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_Chrome"))
			: nullptr;
		if (_basicButton && !basicChrome)
			return fail(L"Generic.xaml 未生成 basicButton 的 PART_Chrome："
				L"Theme=" + std::wstring(
					cui::framework::StyleAccess::Theme(*_basicButton)
						? L"存在" : L"缺失")
				+ L"，Template="
				+ (_basicButton->GetTemplate() ? L"存在" : L"缺失")
				+ L"，TemplateSource="
				+ std::to_wstring(static_cast<int>(
					_basicButton->GetPropertyValueSource(
						Control::TemplateProperty()))));
		const wchar_t* failedThemeInvariant = nullptr;
		if (!_basicButton) failedThemeInvariant = L"basicButton";
		else if (!themeNormalButton) failedThemeInvariant = L"themeNormalButton";
		else if (!themeDisabledButton) failedThemeInvariant = L"themeDisabledButton";
		else if (!basicChrome) failedThemeInvariant = L"basic PART_Chrome";
		else if (!basicPresenter)
			failedThemeInvariant = L"basic PART_ContentPresenter";
		else if (!normalChrome) failedThemeInvariant = L"normal PART_Chrome";
		else if (!disabledChrome) failedThemeInvariant = L"disabled PART_Chrome";
		else if (!basicRoot)
			failedThemeInvariant = L"basic template root";
		else if (basicRoot->GetVisualParent() != _basicButton)
			failedThemeInvariant = L"basic root visual parent";
		else if (basicRoot->GetLogicalParent() != nullptr)
			failedThemeInvariant = L"basic root logical parent";
		else if (basicRoot->GetTemplatedParent() != _basicButton)
			failedThemeInvariant = L"basic root templated parent";
		else if (basicChrome->GetVisualParent() != basicRoot)
			failedThemeInvariant = L"basic chrome visual parent";
		else if (basicChrome->GetLogicalParent() != nullptr)
			failedThemeInvariant = L"basic chrome logical parent";
		else if (basicChrome->GetTemplatedParent() != _basicButton)
			failedThemeInvariant = L"basic chrome templated parent";
		else if (basicPresenter->GetVisualParent() != basicChrome)
			failedThemeInvariant = L"basic presenter visual parent";
		else if (basicPresenter->GetTemplatedParent() != _basicButton)
			failedThemeInvariant = L"basic presenter templated parent";
		else if (basicPresenter->GetPropertyValueSource(
			ContentPresenter::ContentProperty())
			!= DependencyPropertyValueSource::Template)
			failedThemeInvariant = L"presenter Content source";
		else if (basicPresenter->GetPropertyExpressionKind(
			ContentPresenter::ContentProperty(),
			DependencyPropertyValueSource::Template)
			!= DependencyPropertyExpressionKind::TemplateBinding)
			failedThemeInvariant = L"presenter Content TemplateBinding";
		else if (_basicButton->GetPropertyValueSource(
			Control::BackgroundProperty())
			!= DependencyPropertyValueSource::Style)
			failedThemeInvariant = L"basic Background source";
		else if (_basicButton->GetPropertyValueSource(
			Control::BorderBrushProperty())
			!= DependencyPropertyValueSource::Theme)
			failedThemeInvariant = L"basic BorderBrush source";
		else if (basicChrome->GetPropertyValueSource(
			Border::BackgroundProperty())
			!= DependencyPropertyValueSource::Template)
			failedThemeInvariant = L"chrome Background source";
		else if (themeNormalButton->GetPropertyValueSource(
			Control::BackgroundProperty())
			!= DependencyPropertyValueSource::Theme)
			failedThemeInvariant = L"normal Background source";
		else if (themeNormalButton->GetCurrentVisualState(
			MakeVisualStateGroupToken(L"CommonStates"))
			!= MakeVisualStateToken(L"Normal"))
			failedThemeInvariant = L"normal CommonStates";
		else if (themeDisabledButton->GetCurrentVisualState(
			MakeVisualStateGroupToken(L"CommonStates"))
			!= MakeVisualStateToken(L"Disabled"))
			failedThemeInvariant = L"disabled CommonStates";
		else if (disabledChrome->GetPropertyValueSource(
			Border::BackgroundProperty())
			!= DependencyPropertyValueSource::VisualState)
			failedThemeInvariant = L"disabled chrome Background source";
		if (failedThemeInvariant)
			return fail(L"Generic.xaml Theme/ControlTemplate/TemplateBinding/"
				L"VisualState 主链未完整物化：" + std::wstring(failedThemeInvariant));

		auto* themeCheck = dynamic_cast<CheckBox*>(
			FindGeneratedControlByName(L"enableInput"));
		auto* verticalSlider = dynamic_cast<Slider*>(
			FindGeneratedControlByName(
				L"verticalThemeSlider"));
		auto* verticalProgress = dynamic_cast<ProgressBar*>(
			FindGeneratedControlByName(
				L"verticalThemeProgress"));
		auto* indeterminateProgress = dynamic_cast<ProgressBar*>(
			FindGeneratedControlByName(
				L"indeterminateProgress"));
		if (!applyTemplateForVerification(themeCheck, L"enableInput")
			|| !applyTemplateForVerification(_radioA, L"radioA")
			|| !applyTemplateForVerification(_progress, L"progress")
			|| !applyTemplateForVerification(
				indeterminateProgress, L"indeterminateProgress")
			|| !applyTemplateForVerification(
				_globalProgress, L"globalProgress")
			|| !applyTemplateForVerification(
				verticalSlider, L"verticalThemeSlider")
			|| !applyTemplateForVerification(
				verticalProgress, L"verticalThemeProgress"))
			return false;
		auto* checkRoot = themeCheck
			? themeCheck->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_CheckBoxRoot")) : nullptr;
		auto* checkGlyph = themeCheck
			? themeCheck->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_CheckGlyph")) : nullptr;
		auto* radioRoot = _radioA
			? _radioA->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_RadioRoot")) : nullptr;
		auto* radioGlyph = _radioA
			? _radioA->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_RadioGlyph")) : nullptr;
		auto* progressTrack = _progress
			? _progress->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_Track")) : nullptr;
		auto* progressIndicator = _progress
			? _progress->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_Indicator")) : nullptr;
		auto* progressGlow = indeterminateProgress
			? indeterminateProgress->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_GlowRect")) : nullptr;
		auto* sliderTrack = _globalProgress
			? _globalProgress->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_Track")) : nullptr;
		auto* sliderRange = _globalProgress
			? _globalProgress->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_SelectionRange")) : nullptr;
		auto* sliderThumb = _globalProgress
			? _globalProgress->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_Thumb")) : nullptr;
		bool indeterminateStateMatches = indeterminateProgress
			&& indeterminateProgress->GetCurrentVisualState(
				MakeVisualStateGroupToken(L"ProgressStates"))
				== MakeVisualStateToken(L"Indeterminate");
		if (!indeterminateStateMatches && indeterminateProgress && _tabs)
		{
			TabItem* ownerTab = nullptr;
			for (auto* current = indeterminateProgress->GetVisualParent();
				current && !ownerTab; current = current->GetVisualParent())
				ownerTab = dynamic_cast<TabItem*>(current);
			const int ownerIndex = _tabs->IndexOfItem(ownerTab);
			const int previousIndex = _tabs->SelectedIndex;
			if (ownerIndex >= 0)
			{
				_tabs->SelectedIndex = ownerIndex;
				RequestLayout();
				UpdateLayout();
				indeterminateStateMatches =
					indeterminateProgress->GetCurrentVisualState(
						MakeVisualStateGroupToken(L"ProgressStates"))
					== MakeVisualStateToken(L"Indeterminate");
				_tabs->SelectedIndex = previousIndex;
				RequestLayout();
				UpdateLayout();
			}
		}
		const wchar_t* failedControlThemeInvariant = nullptr;
		if (!themeCheck) failedControlThemeInvariant = L"enableInput";
		else if (!verticalSlider) failedControlThemeInvariant = L"verticalSlider";
		else if (!verticalProgress) failedControlThemeInvariant = L"verticalProgress";
		else if (!indeterminateProgress) failedControlThemeInvariant = L"indeterminateProgress";
		else if (!checkRoot) failedControlThemeInvariant = L"checkRoot";
		else if (!checkGlyph) failedControlThemeInvariant = L"checkGlyph";
		else if (!radioRoot) failedControlThemeInvariant = L"radioRoot";
		else if (!radioGlyph) failedControlThemeInvariant = L"radioGlyph";
		else if (!progressTrack) failedControlThemeInvariant = L"progressTrack";
		else if (!progressIndicator) failedControlThemeInvariant = L"progressIndicator";
		else if (!progressGlow) failedControlThemeInvariant = L"progressGlow";
		else if (!sliderTrack) failedControlThemeInvariant = L"sliderTrack";
		else if (!sliderRange) failedControlThemeInvariant = L"sliderRange";
		else if (!sliderThumb) failedControlThemeInvariant = L"sliderThumb";
		else if (themeCheck->GetPropertyValueSource(
			Control::BorderBrushProperty()) != DependencyPropertyValueSource::Theme)
			failedControlThemeInvariant = L"CheckBox BorderBrush source";
		else if (_radioA->GetPropertyValueSource(
			Control::ForegroundProperty()) != DependencyPropertyValueSource::Theme)
			failedControlThemeInvariant = L"RadioButton Foreground source";
		else if (_progress->GetPropertyValueSource(
			Control::BackgroundProperty()) != DependencyPropertyValueSource::Theme)
			failedControlThemeInvariant = L"ProgressBar Background source";
		else if (_globalProgress->GetPropertyValueSource(
			Control::BackgroundProperty()) != DependencyPropertyValueSource::Theme)
			failedControlThemeInvariant = L"Slider Background source";
		else if (checkRoot->GetTemplatedParent() != themeCheck)
			failedControlThemeInvariant = L"checkRoot templated parent";
		else if (radioRoot->GetTemplatedParent() != _radioA)
			failedControlThemeInvariant = L"radioRoot templated parent";
		else if (progressTrack->GetTemplatedParent() != _progress)
			failedControlThemeInvariant = L"progressTrack templated parent";
		else if (sliderTrack->GetTemplatedParent() != _globalProgress)
			failedControlThemeInvariant = L"sliderTrack templated parent";
		else if (themeCheck->GetCurrentVisualState(
			MakeVisualStateGroupToken(L"CheckStates"))
			!= MakeVisualStateToken(L"Checked"))
			failedControlThemeInvariant = L"CheckBox Checked state";
		else if (_radioA->GetCurrentVisualState(
			MakeVisualStateGroupToken(L"CheckStates"))
			!= MakeVisualStateToken(L"Checked"))
			failedControlThemeInvariant = L"RadioButton Checked state";
		else if (!indeterminateStateMatches)
			failedControlThemeInvariant = L"ProgressBar Indeterminate state";
		else if (verticalSlider->Orientation != Orientation::Vertical)
			failedControlThemeInvariant = L"Slider Orientation";
		else if (verticalSlider->GetCurrentVisualState(
			MakeVisualStateGroupToken(L"OrientationStates"))
			!= MakeVisualStateToken(L"Vertical"))
			failedControlThemeInvariant = L"Slider Vertical state";
		else if (verticalProgress->Orientation != Orientation::Vertical)
			failedControlThemeInvariant = L"ProgressBar Orientation";
		else if (verticalProgress->GetCurrentVisualState(
			MakeVisualStateGroupToken(L"OrientationStates"))
			!= MakeVisualStateToken(L"Vertical"))
			failedControlThemeInvariant = L"ProgressBar Vertical state";
		if (failedControlThemeInvariant)
			return fail(L"Generic.xaml CheckBox/RadioButton/ProgressBar/"
				L"Slider 模板、部件或 VisualState 未完整物化："
				+ std::wstring(failedControlThemeInvariant));

		const bool pointerDown = cui::framework::InputAccess::DispatchInput(
			*themeNormalButton, PointerInput(
				InputReportKind::PointerDown, MouseButton::Left,
				5, 5, MouseButton::Left));
		const bool pressedState = themeNormalButton->IsPressed
			&& themeNormalButton->GetCurrentVisualState(
				MakeVisualStateGroupToken(L"CommonStates"))
				== MakeVisualStateToken(L"Pressed");
		const bool pointerUp = cui::framework::InputAccess::DispatchInput(
			*themeNormalButton, PointerInput(
				InputReportKind::PointerUp, MouseButton::Left, 5, 5));
		if (!pointerDown || !pressedState || !pointerUp
			|| themeNormalButton->IsPressed
			|| themeNormalButton->GetCurrentVisualState(
				MakeVisualStateGroupToken(L"CommonStates"))
				!= MakeVisualStateToken(L"Normal")
			|| normalChrome->GetPropertyValueSource(Border::BackgroundProperty())
				!= DependencyPropertyValueSource::Template)
			return fail(L"Generic.xaml Button CommonStates 未响应 "
				L"Pressed/Normal 状态切换。");

		if (!_basicButton || !_dialogCancelButton || !_radioA
			|| !_basicButton->IsDefault || _basicButton->IsCancel
			|| _dialogCancelButton->IsDefault
			|| !_dialogCancelButton->IsCancel
			|| _radioA->GroupName != L"Plan"
			|| _radioB->GroupName != L"Plan"
			|| !_radioA->IsChecked || _radioB->IsChecked
			|| _basicButton->GetPropertyValueSource(Button::IsDefaultProperty())
				!= DependencyPropertyValueSource::Local
			|| _dialogCancelButton->GetPropertyValueSource(
				Button::IsCancelProperty())
				!= DependencyPropertyValueSource::Local)
			return fail(L"Button IsDefault/IsCancel 未从 XAML 本地值物化。");

		const auto previousFocus = GetKeyboardFocusedElement();
		const auto defaultText = _basicButton->GetDisplayText();
		const auto cancelText = _dialogCancelButton->GetDisplayText();
		const auto defaultTag = _basicButton->Tag;
		const auto cancelTag = _dialogCancelButton->Tag;
		const auto statusText = _statusText ? _statusText->Text : std::wstring{};
		const auto statusPart = GetStatusBarItemText(0);
		int defaultInvocations = 0;
		int cancelInvocations = 0;
		int radioInvocations = 0;
		auto defaultConnection = _basicButton->Click.Subscribe(
			[&](Control*, RoutedEventArgs&) { ++defaultInvocations; });
		auto cancelConnection = _dialogCancelButton->Click.Subscribe(
			[&](Control*, RoutedEventArgs&) { ++cancelInvocations; });
		auto radioConnection = _radioA->Click.Subscribe(
			[&](Control*, RoutedEventArgs&) { ++radioInvocations; });
		auto pressKey = [this](Key key)
		{
			(void)cui::framework::InputAccess::DispatchInput(
				*this, KeyInput(InputReportKind::KeyDown, key));
			(void)cui::framework::InputAccess::DispatchInput(
				*this, KeyInput(InputReportKind::KeyUp, key));
		};

		SetKeyboardFocus(_radioA, false);
		const bool genericFocused = GetKeyboardFocusedElement() == _radioA;
		pressKey(Key::Return);
		const bool enterUsesDefault = defaultInvocations == 1
			&& cancelInvocations == 0 && radioInvocations == 0;
		pressKey(Key::Escape);
		const bool escapeUsesCancel = defaultInvocations == 1
			&& cancelInvocations == 1 && radioInvocations == 0;
		pressKey(Key::Space);
		const bool radioSpaceInvoked = defaultInvocations == 1
			&& cancelInvocations == 1 && radioInvocations == 1;
		SetKeyboardFocus(_basicButton, false);
		const bool buttonFocused = GetKeyboardFocusedElement() == _basicButton;
		pressKey(Key::Space);
		const bool buttonSpaceInvoked = defaultInvocations == 2
			&& cancelInvocations == 1 && radioInvocations == 1;

		cui::framework::WindowAccess::TextComposition(*this).Reset();
		SetKeyboardFocus(previousFocus, false);
		_basicButton->SetContent(BindingValue(defaultText));
		_basicButton->Tag = defaultTag;
		_basicButton->InvalidateVisual();
		_dialogCancelButton->SetContent(BindingValue(cancelText));
		_dialogCancelButton->Tag = cancelTag;
		_dialogCancelButton->InvalidateVisual();
		if (_statusText)
		{
			_statusText->Text = statusText;
			_statusText->InvalidateVisual();
		}
		(void)SetStatusBarItemText(0, statusPart);
		if (!genericFocused || !enterUsesDefault || !escapeUsesCancel
			|| !radioSpaceInvoked || !buttonFocused || !buttonSpaceInvoked)
			return fail(L"Window 未按 WPF Button 语义路由 Enter/Escape/Space："
				L"genericFocused=" + std::to_wstring(genericFocused)
				+ L"，enterUsesDefault=" + std::to_wstring(enterUsesDefault)
				+ L"，escapeUsesCancel=" + std::to_wstring(escapeUsesCancel)
				+ L"，radioSpaceInvoked=" + std::to_wstring(radioSpaceInvoked)
				+ L"，buttonFocused=" + std::to_wstring(buttonFocused)
				+ L"，buttonSpaceInvoked=" + std::to_wstring(buttonSpaceInvoked)
				+ L"，basicWindow="
				+ std::to_wstring(_basicButton->GetPresentationWindow() == this)
				+ L"，cancelWindow="
				+ std::to_wstring(
					_dialogCancelButton->GetPresentationWindow() == this)
				+ L"，radioWindow="
				+ std::to_wstring(_radioA->GetPresentationWindow() == this)
				+ L"，basicVisible=" + std::to_wstring(_basicButton->IsVisible)
				+ L"，cancelVisible="
				+ std::to_wstring(_dialogCancelButton->IsVisible)
				+ L"，radioVisible=" + std::to_wstring(_radioA->IsVisible));

		auto* card = featureCard;
		if (!card
			|| card->GetCaption()
				!= L"FeatureCard · 类型/属性/事件均来自 XAML"
			|| !card->GetIsActive())
			return fail(L"featureCard 未生成强类型属性投影。");
		auto* statePart = card->GetPART_State();
		auto* rootPart = card->GetPART_Root();
		auto* contentPresenter = card->GetPART_Content();
		auto* actionsPresenter = card->GetPART_Actions();
		auto* projectedContent = FindGeneratedControlByName(
			L"featureCardContent");
		auto* projectedActionA = FindGeneratedControlByName(
			L"featureActionA");
		auto* projectedActionB = FindGeneratedControlByName(
			L"featureActionB");
		auto* templateLayout = statePart ? statePart->GetVisualParent() : nullptr;
		if (!statePart || !rootPart
			|| !contentPresenter || contentPresenter->VisualChildCount() != 1
			|| !actionsPresenter || actionsPresenter->VisualChildCount() != 2
			|| !projectedContent || !projectedActionA || !projectedActionB
			|| !templateLayout || templateLayout == rootPart)
			return fail(L"FeatureCard 默认/多值内容槽未实际投影。");
		const auto cardLogicalChildren = card->GetLogicalChildrenView();
		if (rootPart->GetVisualParent() != card
			|| rootPart->GetLogicalParent() != nullptr
			|| rootPart->GetTemplatedParent() != card
			|| rootPart->GetInheritanceParent() != card
			|| rootPart->GetRoutedParent() != card
			|| templateLayout->GetVisualParent() != rootPart
			|| templateLayout->GetLogicalParent() != nullptr
			|| templateLayout->GetTemplatedParent() != card
			|| templateLayout->GetInheritanceParent() != rootPart
			|| statePart->GetVisualParent() != templateLayout
			|| statePart->GetLogicalParent() != nullptr
			|| statePart->GetTemplatedParent() != card
			|| statePart->GetInheritanceParent() != templateLayout
			|| contentPresenter->GetVisualParent() != templateLayout
			|| contentPresenter->GetLogicalParent() != nullptr
			|| contentPresenter->GetTemplatedParent() != card
			|| contentPresenter->GetInheritanceParent() != templateLayout
			|| actionsPresenter->GetVisualParent() != templateLayout
			|| actionsPresenter->GetLogicalParent() != nullptr
			|| actionsPresenter->GetTemplatedParent() != card
			|| actionsPresenter->GetInheritanceParent() != templateLayout
			|| projectedContent->GetVisualParent() != contentPresenter
			|| projectedContent->GetLogicalParent() != card
			|| projectedContent->GetTemplatedParent() != nullptr
			|| projectedActionA->GetVisualParent() != actionsPresenter
			|| projectedActionA->GetLogicalParent() != card
			|| projectedActionB->GetVisualParent() != actionsPresenter
			|| projectedActionB->GetLogicalParent() != card
			|| std::find(cardLogicalChildren.begin(), cardLogicalChildren.end(),
				rootPart) != cardLogicalChildren.end()
			|| std::find(cardLogicalChildren.begin(), cardLogicalChildren.end(),
				projectedContent) == cardLogicalChildren.end()
			|| std::find(cardLogicalChildren.begin(), cardLogicalChildren.end(),
				projectedActionA) == cardLogicalChildren.end()
			|| projectedContent->GetDisplayText()
				!= L"Logical DataContext via FeatureCard: Ada")
			return fail(L"FeatureCard 的 Visual/Logical/Template 三树关系未分离。");

		auto* classCommandSource = dynamic_cast<Button*>(projectedActionB);
		const auto classCommandInputs = classCommandSource
			? classCommandSource->GetInputBindings()
			: std::span<const InputBinding>{};
		const auto* classCommandMouse = classCommandInputs.size() == 1
			? std::get_if<MouseBinding>(&classCommandInputs.front()) : nullptr;
		if (!classCommandSource || !classCommandMouse
			|| classCommandMouse->Command.Name() != L"Demo.Component.ClassProbe"
			|| CommandParameterText(classCommandMouse->CommandParameter)
				!= L"feature-class-button"
			|| classCommandMouse->CommandTarget != card)
			return fail(L"FeatureCard class command source 未由 XAML 声明。");
		_classCommandEnabled = true;
		_classCommandTrace.clear();
		_classCommandCanExecuteCount = 0;
		_classCommandExecutedCount = 0;
		_nativeClassCommandCanExecuteCount = 0;
		_nativeClassCommandExecutedCount = 0;
		_lastCommandParameter.clear();
		const bool classCommandInvoked = cui::framework::InputAccess::ProcessCommandInput(
			*classCommandSource,
			MouseEventArgs(MouseButton::Left, MouseButtonState::Pressed, 1, 0, 0, 0));
		if (!classCommandInvoked
			|| _classCommandCanExecuteCount != 1
			|| _classCommandExecutedCount != 1
			|| _nativeClassCommandCanExecuteCount != 0
			|| _nativeClassCommandExecutedCount != 0
			|| _classCommandTrace != std::vector<std::wstring>{
				L"QName.CanExecute", L"QName.Executed" }
			|| _lastCommandParameter != L"feature-class-button")
		{
			std::wstring classTrace;
			for (const auto& item : _classCommandTrace)
			{
				if (!classTrace.empty()) classTrace += L"|";
				classTrace += item;
			}
			return fail(L"RoutedCommand exact XAML QName class binding 未先于 native fallback："
				L"invoked=" + std::to_wstring(classCommandInvoked)
				+ L", qname can/exec="
				+ std::to_wstring(_classCommandCanExecuteCount) + L"/"
				+ std::to_wstring(_classCommandExecutedCount)
				+ L", native can/exec="
				+ std::to_wstring(_nativeClassCommandCanExecuteCount) + L"/"
				+ std::to_wstring(_nativeClassCommandExecutedCount)
				+ L", parameter=" + _lastCommandParameter
				+ L", trace=" + classTrace + L"。");
		}

		_classCommandEnabled = false;
		_classCommandTrace.clear();
		_classCommandCanExecuteCount = 0;
		_classCommandExecutedCount = 0;
		_nativeClassCommandCanExecuteCount = 0;
		_nativeClassCommandExecutedCount = 0;
		const bool disabledClassInputConsumed =
			cui::framework::InputAccess::ProcessCommandInput(
				*classCommandSource,
				MouseEventArgs(MouseButton::Left, MouseButtonState::Pressed, 1, 0, 0, 0));
		if (!disabledClassInputConsumed
			|| _classCommandCanExecuteCount != 1
			|| _classCommandExecutedCount != 0
			|| _nativeClassCommandCanExecuteCount == 0
			|| _nativeClassCommandExecutedCount != 0
			|| _classCommandTrace.empty()
			|| _classCommandTrace.front() != L"QName.CanExecute"
			|| std::any_of(
				_classCommandTrace.begin() + 1, _classCommandTrace.end(),
				[](const std::wstring& item)
				{ return item != L"NativeFrameworkElement.CanExecute"; }))
			return fail(L"QName class binding 禁用路径未继续进入 native fallback。");

		_classCommandEnabled = true;
		int observerNotifications = 0;
		bool observedCanExecute = false;
		std::uint64_t observedGeneration = 0;
		int requeryNotifications = 0;
		std::uint64_t suggestedGeneration = 0;
		auto observerConnection = RoutedCommandManager::ObserveCanExecute(
			*classCommandSource,
			RoutedCommandSourceQuery{
				RoutedCommand(L"Demo.Component.ClassProbe"),
				std::wstring(L"observer"), card },
			[&](Control&, const RoutedCommandCanExecuteResult& result)
			{
				++observerNotifications;
				observedCanExecute = result.CanExecute;
				observedGeneration = result.RequeryGeneration;
			});
		auto classRequeryConnection =
			RoutedCommandManager::SubscribeRequerySuggested(
				*classCommandSource,
				[&](const RoutedCommandRequeryEventArgs& args)
				{
					if (args.Scope == this)
					{
						++requeryNotifications;
						suggestedGeneration = args.Generation;
					}
				});
		const auto generationBefore =
			RoutedCommandManager::GetRequeryGeneration(*classCommandSource);
		_classCommandEnabled = false;
		const bool invalidated =
			RoutedCommandManager::InvalidateRequerySuggested(*classCommandSource);
		cui::PumpUIThreadCallbacks();
		const auto generationAfter =
			RoutedCommandManager::GetRequeryGeneration(*classCommandSource);
		if (!observerConnection.Connected() || !classRequeryConnection.Connected()
			|| !invalidated || observerNotifications != 2
			|| observedCanExecute || generationAfter <= generationBefore
			|| observedGeneration != generationAfter
			|| requeryNotifications != 1
			|| suggestedGeneration != generationAfter)
			return fail(L"Window scoped requery 未自动刷新统一 CanExecute observer。");
		_classCommandEnabled = true;
		(void)RoutedCommandManager::InvalidateRequerySuggested(*classCommandSource);
		cui::PumpUIThreadCallbacks();
		if (observerNotifications != 3 || !observedCanExecute)
			return fail(L"CanExecute observer 未在下一代 requery 恢复 enabled。");

		const auto templateInputBindings = rootPart->GetInputBindings();
		const auto* templateKeyBinding = templateInputBindings.empty()
			? nullptr : std::get_if<KeyBinding>(&templateInputBindings.front());
		_classCommandTrace.clear();
		_classCommandCanExecuteCount = 0;
		_classCommandExecutedCount = 0;
		_nativeClassCommandCanExecuteCount = 0;
		_nativeClassCommandExecutedCount = 0;
		_lastCommandParameter.clear();
		const auto controlF10 = Key::F10;
		if (templateInputBindings.size() != 1 || !templateKeyBinding
			|| templateKeyBinding->CommandTarget != rootPart
			|| !cui::framework::InputAccess::ProcessCommandInput(
				*rootPart, KeyEventArgs(
					controlF10, ModifierKeys::Control))
			|| _classCommandCanExecuteCount != 0
			|| _classCommandExecutedCount != 0
			|| _nativeClassCommandCanExecuteCount != 1
			|| _nativeClassCommandExecutedCount != 1
			|| _lastCommandParameter != L"template-native-input"
			|| _classCommandTrace != std::vector<std::wstring>{
				L"NativeFrameworkElement.CanExecute",
				L"NativeFrameworkElement.Executed" })
			return fail(L"组件模板 InputBinding.CommandTarget 未按本地 namescope "
				L"重写，或 native class binding fallback 未执行。");
		const auto state = card->GetState();
		if (state.find(L"Typed C++ attached") == std::wstring::npos
			|| statePart->Text != state
			|| statePart->GetPropertyValueSource(Label::TextProperty())
				!= DependencyPropertyValueSource::Template
			|| statePart->GetPropertyExpressionKind(
				Label::TextProperty(), DependencyPropertyValueSource::Template)
				!= DependencyPropertyExpressionKind::TemplateBinding
			|| rootPart->GetPropertyValueSource(Border::PaddingProperty())
				!= DependencyPropertyValueSource::Template
			|| rootPart->GetPropertyExpressionKind(
				Border::PaddingProperty(),
				DependencyPropertyValueSource::Template)
				!= DependencyPropertyExpressionKind::TemplateBinding
			|| rootPart->GetPropertyMetadata(
				Border::CornerRadiusProperty()) == nullptr)
			return fail(L"FeatureCard 只读 State、TemplateBinding 或 "
				L"Border.CornerRadius DP 未生效。");
		if (card->TrySetPropertyValue(
			DemoWindowGeneratedFeatureCard::StateProperty(),
			BindingValue(std::wstring(L"illegal write"))))
			return fail(L"FeatureCard State 未保持只读契约。");
		if (card->GetCurrentVisualState(
			MakeVisualStateGroupToken(L"InteractionStates"))
			!= MakeVisualStateToken(L"Active"))
			return fail(L"FeatureCard IsActive StateTrigger 未进入 Active。");
		card->SetIsActive(false);
		const bool deactivated = !card->GetIsActive();
		const auto inactiveState = card->GetCurrentVisualState(
			MakeVisualStateGroupToken(L"InteractionStates"));
		card->SetIsActive(true);
		const bool activated = card->GetIsActive();
		const auto activeState = card->GetCurrentVisualState(
			MakeVisualStateGroupToken(L"InteractionStates"));
		const bool hasTransition = card->HasActiveVisualStateAnimations();
		const auto transitionSource = rootPart->GetPropertyValueSource(
			Control::CanvasLeftProperty());
		const auto transitionExpression = rootPart->GetPropertyExpressionKind(
			Control::CanvasLeftProperty(),
			DependencyPropertyValueSource::Animation);
		if (!deactivated || inactiveState != MakeVisualStateToken(L"Normal")
			|| !activated || activeState != MakeVisualStateToken(L"Active")
			|| !hasTransition
			|| transitionSource != DependencyPropertyValueSource::Animation
			|| transitionExpression != DependencyPropertyExpressionKind::Animation)
			return fail(L"FeatureCard VisualTransition 未从 Normal 过渡到 Active。"
				L" [deactivated=" + std::to_wstring(deactivated)
				+ L", inactive=" + std::to_wstring(inactiveState.Value)
				+ L", activated=" + std::to_wstring(activated)
				+ L", active=" + std::to_wstring(activeState.Value)
				+ L", clock=" + std::to_wstring(hasTransition)
				+ L", source=" + std::to_wstring(
					static_cast<int>(transitionSource))
				+ L", expression=" + std::to_wstring(
					static_cast<int>(transitionExpression)) + L"]");
		const bool advancedTransition =
			cui::framework::PresentationAccess::AdvanceVisualStateAnimations(
				*card, ::GetTickCount64() + 500);
		const bool transitionStillActive =
			card->HasActiveVisualStateAnimations();
		const auto transitionLeft = Canvas::GetLeft(*rootPart);
		const auto settledTransitionSource =
			rootPart->GetPropertyValueSource(Control::CanvasLeftProperty());
		if (!advancedTransition || transitionStillActive
			|| std::abs(transitionLeft - 12.0f) > 0.001f
			|| settledTransitionSource
				!= DependencyPropertyValueSource::VisualState)
			return fail(L"FeatureCard VisualTransition 未收敛到 Active Setter："
				L"advanced=" + std::to_wstring(advancedTransition)
				+ L"，active=" + std::to_wstring(transitionStillActive)
				+ L"，left=" + std::to_wstring(transitionLeft)
				+ L"，source=" + std::to_wstring(
					static_cast<int>(settledTransitionSource)));
		const D2D1_COLOR_F localBackColor{ 0.7f, 0.1f, 0.5f, 1.0f };
		rootPart->SetBackground(cui::drawing::Brush(localBackColor));
		if (rootPart->GetPropertyValueSource(Border::BackgroundProperty())
				!= DependencyPropertyValueSource::Local
			|| std::abs(rootPart->Background.Color.r - localBackColor.r) > 0.001f
			|| !rootPart->ClearPropertyValue(Border::BackgroundProperty())
			|| rootPart->GetPropertyValueSource(Border::BackgroundProperty())
				!= DependencyPropertyValueSource::VisualState)
			return fail(L"Local > VisualState > Template 的有效值优先级未闭环。");
		if (!card->RaisePulse()
			|| !card->HasActiveVisualStateAnimations()
			|| rootPart->GetPropertyValueSource(Control::CanvasLeftProperty())
				!= DependencyPropertyValueSource::Animation)
			return fail(L"FeatureCard EventTrigger 未启动关键帧 Storyboard。");
		const auto pulseTick = ::GetTickCount64();
		if (!cui::framework::PresentationAccess::AdvanceVisualStateAnimations(*card, pulseTick + 120)
			|| Canvas::GetLeft(*(rootPart)) <= 12.0f
			|| Canvas::GetLeft(*(rootPart)) >= 24.0f)
			return fail(L"FeatureCard 关键帧 Storyboard 未实际推进。");
		if (!card->RaiseStopPulse()
			|| !card->HasActiveVisualStateAnimations()
			|| rootPart->GetPropertyValueSource(Control::CanvasLeftProperty())
				!= DependencyPropertyValueSource::Animation)
			return fail(L"FeatureCard StopStoryboard 未保持 next-tick 控制语义。");
		if (!cui::framework::PresentationAccess::AdvanceVisualStateAnimations(
				*card, pulseTick + 121)
			|| card->HasActiveVisualStateAnimations()
			|| std::abs(Canvas::GetLeft(*(rootPart)) - 12.0f) > 0.001f
			|| rootPart->GetPropertyValueSource(Control::CanvasLeftProperty())
				!= DependencyPropertyValueSource::Animation
			|| rootPart->GetPropertyExpressionKind(
				Control::CanvasLeftProperty(),
				DependencyPropertyValueSource::Animation)
				!= DependencyPropertyExpressionKind::Animation)
			return fail(L"FeatureCard StopStoryboard 未在下一 tick 恢复 VisualState 值并保留可控动画源。");

		const auto sourceBefore = _featureInvocations;
		const auto bubbleBefore = _featureBubbleInvocations;
		if (!card->RaiseInvoked()
			|| _featureInvocations != sourceBefore + 1
			|| _featureBubbleInvocations != bubbleBefore + 1)
			return fail(L"FeatureCard Invoked 源处理或冒泡路由未闭环。");
		if (card->GetCurrentVisualState(
			MakeVisualStateGroupToken(L"InteractionStates"))
			!= MakeVisualStateToken(L"Invoked"))
			return fail(L"FeatureCard Invoked EventTrigger 未切换视觉状态。");
		const auto inputsBefore = _featureInputCount;
		if (!cui::framework::InputAccess::DispatchInput(*card, PointerInput(
				InputReportKind::PointerDoubleClick, MouseButton::Left,
				17, 29, MouseButton::Left))
			|| _featureInputCount != inputsBefore + 1)
			return fail(L"FeatureCard 强类型输入处理未接收规范化宿主输入。");
		if (card->GetState().find(L"Typed input") == std::wstring::npos)
			return fail(L"FeatureCard 强类型输入处理未能发布只读 State。");

		{
			struct NativeSurfaceTabScope final
			{
				TabControl& Tabs;
				int SelectedIndex;
				~NativeSurfaceTabScope()
				{
					(void)Tabs.SelectItem(SelectedIndex);
				}
			};
			if (!_tabs)
				return fail(L"NativeSurface 可见输入验证缺少主 TabControl。");
			NativeSurfaceTabScope restoreTab{ *_tabs, _tabs->SelectedIndex };
			if (!_tabs->SelectItem(PageIndex(DemoPage::Containers)))
				return fail(L"无法切换到容器页验证 NativeSurface 输入。");

			auto* scene = dynamic_cast<NativeSurface*>(
				FindGeneratedControlByName(L"demoScene"));
			auto* sceneBehavior = scene ? dynamic_cast<DemoSceneBehavior*>(
				scene->Behavior()) : nullptr;
			if (!scene || scene->GetBehaviorKey() != L"DemoScene"
				|| !sceneBehavior || !sceneBehavior->Attached())
				return fail(L"NativeSurface DemoScene behavior 未实际挂接。");
			const auto inputBefore = sceneBehavior->InputCount();
			(void)cui::framework::InputAccess::DispatchInput(*scene, PointerInput(
				InputReportKind::PointerDown, MouseButton::Left,
				31, 47, MouseButton::Left));
			if (sceneBehavior->InputCount() != inputBefore + 1)
				return fail(L"NativeSurface 未将规范化输入交给 C++ behavior。");
			const std::wstring surfaceText = L"\u03A9\U0001F680";
			if (!cui::framework::WindowAccess::TextComposition(*this).CommitText(
				surfaceText, scene, TextCompositionInputKind::Programmatic)
				|| sceneBehavior->InputCount() != inputBefore + 2
				|| sceneBehavior->LastTextInput() != surfaceText)
				return fail(L"NativeSurface 未消费统一的 std::wstring TextInput。");

			if (!_tabs->SelectItem(PageIndex(DemoPage::Presentation)))
				return fail(L"无法切换到 Presentation 页验证 NativeSurface 输入。");
			auto* presentationSurface = dynamic_cast<NativeSurface*>(
				FindGeneratedControlByName(
					L"presentationProbeSurface"));
			auto* presentationBehavior = presentationSurface
				? dynamic_cast<PresentationProbeBehavior*>(
					presentationSurface->Behavior()) : nullptr;
			auto* presentationStatus = dynamic_cast<Label*>(
				FindGeneratedControlByName(L"presentationStatus"));
			if (!presentationSurface || !presentationStatus
				|| presentationSurface->GetBehaviorKey() != L"PresentationProbe"
				|| !presentationBehavior || !presentationBehavior->Attached()
				|| presentationSurface->GetPresentationWindow() != this
				|| !Handle)
				return fail(L"PresentationRenderHost 可见实验未挂接到真实 Window behavior。");
			RequestLayout();
			UpdateLayout();
			const auto presentationBounds =
				presentationSurface->GetAbsoluteBoundsDip();
			const int presentationHitX = static_cast<int>(std::floor(
				(presentationBounds.left + presentationBounds.right) * 0.5f));
			const int presentationHitY = static_cast<int>(std::floor(
				(presentationBounds.top + presentationBounds.bottom) * 0.5f));
			auto* presentationHit =
				cui::framework::WindowAccess::HitTestControlAt(
					*this, presentationHitX, presentationHitY);
			if (presentationHit != presentationSurface)
				return fail(L"Presentation 可见探针被装饰层遮挡，"
					L"Window hit-test 未命中 NativeSurface。");
			const auto presentationClientBounds =
				ContentDipRectToClientPixels(presentationBounds);
			const LPARAM presentationPoint = MAKELPARAM(
				(presentationClientBounds.left + presentationClientBounds.right) / 2,
				(presentationClientBounds.top + presentationClientBounds.bottom) / 2);
			const int regionRequests = presentationBehavior->RegionRequests();
			(void)::SendMessageW(
				Handle, WM_LBUTTONDOWN, MK_LBUTTON, presentationPoint);
			(void)::SendMessageW(
				Handle, WM_MOUSEMOVE, MK_LBUTTON, presentationPoint);
			(void)::SendMessageW(
				Handle, WM_LBUTTONUP, 0, presentationPoint);
			if (presentationBehavior->RegionRequests() != regionRequests + 2)
				return fail(L"Presentation NativeSurface 未通过真实 Window 指针按下/移动"
					L"提交局部 DIP 脏区。");
		}

		{
			struct DataTabScope final
			{
				TabControl& Tabs;
				int SelectedIndex;
				~DataTabScope()
				{
					(void)Tabs.SelectItem(SelectedIndex);
				}
			};
			if (!_tabs)
				return fail(L"数据控件可见验证缺少主 TabControl。");
			DataTabScope restoreTab{ *_tabs, _tabs->SelectedIndex };
			if (!_tabs->SelectItem(PageIndex(DemoPage::Data)))
				return fail(L"无法切换到数据控件页执行层次容器验证。");

		auto* synchronizedList = dynamic_cast<ListBox*>(
			FindGeneratedControlByName(L"demoListBox"));
		const auto synchronizedSource = synchronizedList
			? synchronizedList->GetItemsSource() : BindingListReference{};
		auto* currentView = synchronizedSource
			? dynamic_cast<IBindingListCurrentView*>(synchronizedSource.Get()) : nullptr;
		if (!synchronizedList
			|| !synchronizedList->GetIsSynchronizedWithCurrentItem()
			|| !currentView
			|| synchronizedList->GetSelectedIndex() != currentView->CurrentPosition())
			return fail(L"ListBox IsSynchronizedWithCurrentItem 未保持初始双端一致：SelectedIndex="
				+ std::to_wstring(synchronizedList
					? synchronizedList->GetSelectedIndex() : -999)
				+ L"，HasSource=" + std::to_wstring(
					synchronizedSource ? 1 : 0)
				+ L"，SourceCount=" + std::to_wstring(
					synchronizedSource
						? synchronizedSource.Get()->Count() : 0)
				+ L"，CurrentPosition=" + std::to_wstring(currentView
					? currentView->CurrentPosition() : -999)
				+ L"，TemplateError=" + (synchronizedList
					? synchronizedList->LastTemplateError() : L"<missing>")
				+ L"。");
		const int originalPosition = currentView->CurrentPosition();
		const int targetPosition = synchronizedSource.Get()->Count() > 1
			? (originalPosition == 0 ? 1 : 0) : -1;
		if (!currentView->MoveCurrentToPosition(targetPosition)
			|| synchronizedList->GetSelectedIndex() != targetPosition)
			return fail(L"CollectionViewSource CurrentItem 未同步到 ListBox selection。");
		if (!synchronizedList->SelectIndex(originalPosition)
			|| currentView->CurrentPosition() != originalPosition)
			return fail(L"ListBox selection 未同步回 CollectionViewSource currency。");
		auto* tree = dynamic_cast<TreeView*>(
			FindGeneratedControlByName(L"demoTree"));
		if (!tree || !tree->GetItemsSource()
			|| tree->GetItemsSource().Get()->Count() != 2
			|| tree->ItemCount() != 2)
			return fail(L"TreeView ItemsSource 未从生成绑定连接 DataContext。");
		auto* rootContainer = tree->ContainerFromIndex(0);
		if (!rootContainer
			|| !rootContainer->GetGeneratedHeaderContent()
			|| !rootContainer->HasItems
			|| rootContainer->ItemCount() != 3)
			return fail(L"HierarchicalDataTemplate 根节点或 Children 绑定未生成。");
		rootContainer->SetIsExpanded(true);
		if (rootContainer->ItemCount() != 3
			|| !rootContainer->ContainerFromIndex(0)
			|| tree->GeneratedItemCount() != tree->ItemCount())
			return fail(L"HierarchicalDataTemplate 子节点未实际生成：items="
				+ std::to_wstring(rootContainer->ItemCount())
				+ L"，first=" + std::to_wstring(
					rootContainer->ContainerFromIndex(0) ? 1 : 0)
				+ L"，treeGenerated="
				+ std::to_wstring(tree->GeneratedItemCount())
				+ L"，rootItems=" + std::to_wstring(tree->ItemCount())
				+ L"，templateError=" + rootContainer->LastTemplateError());
		const auto selectionBefore = _treeSelectionChanges;
		if (!tree->SelectItem(rootContainer, false)
			|| _treeSelectionChanges != selectionBefore + 1
			|| tree->GetSelectedValue().ToString() != L"Workspace")
			return fail(L"TreeView SelectedItemChanged 命名处理未闭环。");

		auto* authoredTree = dynamic_cast<TreeView*>(
			FindGeneratedControlByName(L"authoredStateTree"));
		auto* authoredRoot = authoredTree
			? authoredTree->ContainerFromIndex(0) : nullptr;
		auto* authoredSelected = authoredRoot
			? authoredRoot->ContainerFromIndex(0) : nullptr;
		const auto* expandedMetadata = authoredRoot
			? authoredRoot->GetPropertyMetadata(
				TreeViewItem::IsExpandedProperty()) : nullptr;
		const auto* selectedMetadata = authoredSelected
			? authoredSelected->GetPropertyMetadata(
				TreeViewItem::IsSelectedProperty()) : nullptr;
		if (!authoredTree || !authoredRoot || !authoredSelected
			|| !authoredRoot->IsExpanded
			|| !authoredSelected->IsSelected
			|| authoredTree->GetSelectedContainer() != authoredSelected
			|| authoredRoot->GetPropertyValueSource(
				TreeViewItem::IsExpandedProperty())
				!= DependencyPropertyValueSource::Local
			|| authoredSelected->GetPropertyValueSource(
				TreeViewItem::IsSelectedProperty())
				!= DependencyPropertyValueSource::Local
			|| !expandedMetadata || expandedMetadata->IsReadOnly()
#if CUI_ENABLE_DESIGN_METADATA
			|| expandedMetadata->Design().Persistence
				!= DependencyPropertyPersistence::Metadata
#endif
			|| !selectedMetadata || selectedMetadata->IsReadOnly()
#if CUI_ENABLE_DESIGN_METADATA
			|| selectedMetadata->Design().Persistence
				!= DependencyPropertyPersistence::Metadata
#endif
			)
			return fail(
				L"作者态 TreeViewItem IsExpanded/IsSelected 未形成可写、可持久化的 WPF 容器状态："
				L"tree=" + std::to_wstring(authoredTree ? 1 : 0)
				+ L"，root=" + std::to_wstring(authoredRoot ? 1 : 0)
				+ L"，selected=" + std::to_wstring(authoredSelected ? 1 : 0)
				+ L"，expanded=" + std::to_wstring(
					authoredRoot && authoredRoot->IsExpanded ? 1 : 0)
				+ L"，isSelected=" + std::to_wstring(
					authoredSelected && authoredSelected->IsSelected ? 1 : 0)
				+ L"，selectedContainer=" + std::to_wstring(
					authoredTree && authoredTree->GetSelectedContainer()
						== authoredSelected ? 1 : 0)
				+ L"，expandedSource=" + std::to_wstring(
					authoredRoot
						? static_cast<int>(
							authoredRoot->GetPropertyValueSource(
								TreeViewItem::IsExpandedProperty()))
						: -1)
				+ L"，selectedSource=" + std::to_wstring(
					authoredSelected
						? static_cast<int>(
							authoredSelected->GetPropertyValueSource(
								TreeViewItem::IsSelectedProperty()))
						: -1)
				+ L"，expandedMetadata=" + std::to_wstring(
					expandedMetadata ? 1 : 0)
				+ L"，expandedReadOnly=" + std::to_wstring(
					expandedMetadata && expandedMetadata->IsReadOnly()
						? 1 : 0)
#if CUI_ENABLE_DESIGN_METADATA
				+ L"，expandedPersistence=" + std::to_wstring(
					expandedMetadata
						? static_cast<int>(
							expandedMetadata->Design().Persistence)
						: -1)
#endif
				+ L"，selectedMetadata=" + std::to_wstring(
					selectedMetadata ? 1 : 0)
				+ L"，selectedReadOnly=" + std::to_wstring(
					selectedMetadata && selectedMetadata->IsReadOnly()
						? 1 : 0)
#if CUI_ENABLE_DESIGN_METADATA
				+ L"，selectedPersistence=" + std::to_wstring(
					selectedMetadata
						? static_cast<int>(
							selectedMetadata->Design().Persistence)
						: -1)
#endif
				);

		auto* density = dynamic_cast<ComboBox*>(
			FindGeneratedControlByName(
				L"composedDensityEditor"));
		auto* densitySelected = density ? density->GetItem(1) : nullptr;
		if (!density || density->SelectedIndex != 1 || !densitySelected
			|| !densitySelected->IsSelected
			|| densitySelected->GetPropertyValueSource(
				ItemContainerControl::IsSelectedProperty())
				!= DependencyPropertyValueSource::Local)
			return fail(L"ComboBoxItem.IsSelected 作者态未驱动所属 Selector。");
		}
		{
			auto* grid = dynamic_cast<DataGrid*>(
				FindGeneratedControlByName(L"demoDataGrid"));
			auto* millionButton = dynamic_cast<Button*>(
				FindGeneratedControlByName(L"dataGridMillionButton"));
			auto* orderColumn = grid
				? dynamic_cast<DataGridHyperlinkColumn*>(grid->GetColumn(0)) : nullptr;
			auto* customerColumn = grid
				? dynamic_cast<DataGridTextColumn*>(grid->GetColumn(1)) : nullptr;
			auto* stageColumn = grid
				? dynamic_cast<DataGridComboBoxColumn*>(grid->GetColumn(3)) : nullptr;
			auto* amountColumn = grid
				? dynamic_cast<DataGridTemplateColumn*>(grid->GetColumn(5)) : nullptr;
			auto* paidColumn = grid
				? dynamic_cast<DataGridTemplateColumn*>(grid->GetColumn(6)) : nullptr;
			auto* generatedRegionColumn = grid
				? dynamic_cast<DataGridTextColumn*>(grid->GetColumn(10)) : nullptr;
			if (!grid || !millionButton
				|| millionButton->GetContent().ToString() != L"填充 100 万行"
				|| !grid->GetAutoGenerateColumns()
				|| grid->GetIsReadOnly()
				|| grid->GetCanUserAddRows() != _runtimeDataInitialized
				|| grid->GetCanUserDeleteRows() != _runtimeDataInitialized
				|| !grid->GetCanUserSortColumns()
				|| !grid->GetCanUserResizeColumns()
				|| !grid->GetCanUserReorderColumns()
				|| grid->GetSelectionMode() != SelectionMode::Extended
				|| grid->GetSelectionUnit()
					!= DataGridSelectionUnit::CellOrRowHeader
				|| grid->GetClipboardCopyMode()
					!= DataGridClipboardCopyMode::IncludeHeader
				|| grid->GetHeadersVisibility()
					!= DataGridHeadersVisibility::All
				|| !grid->GetRowValidationErrorTemplate()
				|| (!_runtimeDataInitialized
					&& (grid->GetRowStyle() != L"OrderGridRowStyle"
						|| grid->GetRowStyleSelector()
						|| !grid->GetRowHeaderTemplate()
						|| grid->GetRowHeaderTemplateSelector()
						|| !grid->GetRowDetailsTemplate()
						|| grid->GetRowDetailsTemplateSelector()))
				|| (_runtimeDataInitialized
					&& (!grid->GetRowStyle().empty()
						|| !grid->GetRowStyleSelector()
						|| grid->GetRowHeaderTemplate()
						|| !grid->GetRowHeaderTemplateSelector()
						|| grid->GetRowDetailsTemplate()
						|| !grid->GetRowDetailsTemplateSelector()))
				|| std::abs(grid->GetRowHeaderWidth() - 58.0) > 0.001
				|| std::abs(grid->GetRowHeaderActualWidth() - 58.0) > 0.001
				|| grid->GetSelectedIndex() != -1
				|| !grid->GetSelectedCells().empty()
				|| grid->ColumnCount() != DemoDataGridExpectedColumnCount
				|| !grid->GetItemsSource()
				|| grid->GetItemsSource().Get()->Count() != 18
				|| !orderColumn || !orderColumn->GetIsReadOnly()
				|| orderColumn->GetHeader().ToString() != L"订单号"
				|| orderColumn->GetCompiledBindingPath().Empty()
				|| orderColumn->GetCompiledContentBindingPath().Empty()
				|| orderColumn->GetTargetName() != L"OrderDetails"
				|| !customerColumn
				|| customerColumn->GetBindingMode() != BindingMode::TwoWay
				|| customerColumn->GetWidth().UnitType
					!= DataGridLengthUnitType::Star
				|| !stageColumn || !stageColumn->GetItemsSource()
				|| stageColumn->GetItemsSource().Get()->Count() != 6
				|| stageColumn->GetSelectionBinding()
					!= DataGridComboBoxSelectionBinding::SelectedValue
				|| stageColumn->GetCompiledBindingPath().Empty()
				|| stageColumn->GetCompiledDisplayMemberPath().Empty()
				|| stageColumn->GetCompiledSelectedValuePath().Empty()
				|| !amountColumn || !amountColumn->GetCellTemplate()
				|| !amountColumn->GetCellEditingTemplate()
				|| amountColumn->GetIsReadOnly()
				|| !paidColumn
				|| (!_runtimeDataInitialized && !paidColumn->GetCellTemplate())
				|| (_runtimeDataInitialized
					&& (paidColumn->GetCellTemplate()
						|| !paidColumn->GetCellTemplateSelector()))
				|| !paidColumn->GetIsReadOnly()
				|| paidColumn->GetCanUserResize()
				|| paidColumn->GetCompiledSortMemberPath().Empty()
				|| !generatedRegionColumn
				|| !generatedRegionColumn->GetIsAutoGenerated()
				|| generatedRegionColumn->GetHeader().ToString() != L"AOT 区域"
				|| !generatedRegionColumn->GetIsReadOnly()
				|| generatedRegionColumn->GetCanUserSort()
				|| generatedRegionColumn->GetCanUserResize()
				|| generatedRegionColumn->GetCanUserReorder())
				return fail(
					L"DataGrid 声明式列、静态 Binding、行验证模板、宽度、选择或 ItemsSource 未完整生成。");
			BindingSourceReference firstStage;
			BindingValue firstStageText;
			if (!stageColumn->GetItemsSource().Get()->TryGetItem(0, firstStage)
				|| !firstStage
				|| !firstStage.Get()->TryGetValue(
					MakeBindingSourcePropertyToken(L"Text"), firstStageText)
				|| firstStageText.ToString() != L"待确认")
				return fail(L"DataGrid 状态 ComboBox 列未安装声明式选项数据。");
		}
		auto* fileMenu = _menu ? _menu->GetItem(0) : nullptr;
		auto* helpMenu = _menu ? _menu->GetItem(1) : nullptr;
		auto* openMenuItem = fileMenu ? fileMenu->GetSubItem(0) : nullptr;
		auto* aboutMenuItem = helpMenu ? helpMenu->GetSubItem(0) : nullptr;
		auto* systemSurface = FindGeneratedControlByName(
			L"systemSurface");
		auto* commandTargetButton = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"commandTargetButton"));
		auto* commandTargetTrace = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"commandTargetTrace"));
		const auto runtimeInputBindings = GetInputBindings();
		const auto* openKeyBinding = runtimeInputBindings.empty()
			? nullptr : std::get_if<KeyBinding>(&runtimeInputBindings.front());
		if (!_menu || _menu->ItemCount() != 2 || !fileMenu || !helpMenu
			|| GetInputBindings().size() != 4
			|| !openKeyBinding || openKeyBinding->CommandTarget != _menu
			|| fileMenu->GetDisplayText() != L"文件" || fileMenu->ItemCount() != 3
			|| !openMenuItem
			|| openMenuItem->Command != L"Demo.File.Open"
			|| openMenuItem->CommandParameter != L"menu-open"
			|| openMenuItem->InputGestureText != L"Ctrl+O"
			|| dynamic_cast<Separator*>(fileMenu->GetGeneratedItem(1)) == nullptr
			|| !aboutMenuItem
			|| aboutMenuItem->Command != L"Demo.Help.About"
			|| !systemSurface || !commandTargetButton || !commandTargetTrace
			|| !menuItem7 || menuItem7->CommandTarget != _menu
			|| !menuItem9 || menuItem9->CommandTarget != systemSurface
			|| !commandTargetButton->HasAuthoredCommandTarget()
			|| commandTargetButton->CommandTarget != systemSurface
			|| commandTargetButton->GetPropertyValueSource(
				ButtonBase::CommandTargetProperty())
				!= DependencyPropertyValueSource::Local)
			return fail(L"AOT CommandBindings/InputBindings/MenuItem 命令模型未完整生成："
				+ std::to_wstring(GetInputBindings().size()) + L" inputs, "
				+ std::to_wstring(_menu ? _menu->ItemCount() : 0) + L" menu items。");

		auto resetCommandProbe = [this]()
		{
			_commandRouteTrace.clear();
			_commandPreviewCanExecuteCount = 0;
			_commandCanExecuteCount = 0;
			_commandPreviewExecutedCount = 0;
			_commandExecutedCount = 0;
			_localCommandCanExecuteCount = 0;
			_localCommandExecutedCount = 0;
			_lastCommandParameter.clear();
			_lastCommandCanExecuteTarget.clear();
			_lastCommandExecutedTarget.clear();
			_lastCommandTargetCommand.clear();
			_pendingCommandTargetCommand.clear();
			_displayedCommandCanExecuteTarget.clear();
			_pendingCommandTransactionId = 0;
			_lastCommandExecutedTransactionId = 0;
		};
		auto* commandTargetFocusPeer = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"notifyToggle"));
		TabItem* commandTargetPage = nullptr;
		for (auto* ancestor = commandTargetButton
			? commandTargetButton->GetRoutedParent() : nullptr;
			ancestor; ancestor = ancestor->GetRoutedParent())
			if ((commandTargetPage = dynamic_cast<TabItem*>(ancestor))) break;
		const int commandTargetPageIndex = _tabs && commandTargetPage
			? _tabs->IndexOfItem(commandTargetPage) : -1;
		if (!_tabs || !commandTargetFocusPeer || commandTargetPageIndex < 0)
			return fail(L"CommandTarget 可见实验缺少所属 TabItem 或焦点对照控件。");
		const auto commandTargetFocusBefore = GetKeyboardFocusedElement();
		const auto commandTargetPageBefore = _tabs->SelectedIndex;
		const bool commandTargetPageActivated =
			_tabs->SelectItem(commandTargetPageIndex)
			&& _tabs->SelectedIndex == commandTargetPageIndex;
		SetKeyboardFocus(commandTargetFocusPeer, false);
		const bool commandTargetFocusIsElsewhere =
			GetKeyboardFocusedElement() == commandTargetFocusPeer;
		resetCommandProbe();
		const bool explicitButtonInvoked = commandTargetButton->Invoke();
		const bool explicitButtonIgnoredFocus =
			GetKeyboardFocusedElement() == commandTargetFocusPeer;
		const auto explicitButtonParameter = _lastCommandParameter;
		const auto explicitButtonCanExecuteTarget =
			_lastCommandCanExecuteTarget;
		const auto explicitButtonExecutedTarget =
			_lastCommandExecutedTarget;
		const auto explicitButtonCommand = _lastCommandTargetCommand;
		const auto explicitButtonTrace = commandTargetTrace->Text;
		const bool commandTargetRequeryInvalidated =
			RoutedCommandManager::InvalidateRequerySuggested(*commandTargetButton);
		cui::PumpUIThreadCallbacks();
		const auto explicitButtonTraceAfterRequery = commandTargetTrace->Text;
		_tabs->SelectedIndex = commandTargetPageBefore;
		const bool commandTargetPageRestored =
			_tabs->SelectedIndex == commandTargetPageBefore;
		SetKeyboardFocus(commandTargetFocusBefore, false);
		const bool commandTargetFocusRestored =
			GetKeyboardFocusedElement() == commandTargetFocusBefore;
		const auto explicitButtonTraceAfterRestore = commandTargetTrace->Text;
		if (!commandTargetPageActivated || !commandTargetFocusIsElsewhere
			|| !explicitButtonInvoked
			|| !explicitButtonIgnoredFocus
			|| !commandTargetRequeryInvalidated
			|| !commandTargetPageRestored
			|| !commandTargetFocusRestored
			|| explicitButtonParameter != L"target-button-system-surface"
			|| explicitButtonCanExecuteTarget != L"systemSurface"
			|| explicitButtonExecutedTarget != L"systemSurface"
			|| explicitButtonCommand != L"Demo.System.Refresh"
			|| explicitButtonTrace
				!= L"CanExecute target=systemSurface · Executed target=systemSurface · "
					L"Demo.System.Refresh"
			|| explicitButtonTraceAfterRequery != explicitButtonTrace
			|| explicitButtonTraceAfterRestore != explicitButtonTrace)
			return fail(L"Button.CommandTarget 未脱离当前焦点并从 systemSurface "
				L"完成 CanExecute/Executed 路由。trace="
				+ explicitButtonTrace
				+ L" [invoked=" + std::to_wstring(explicitButtonInvoked)
				+ L", focusElsewhere="
				+ std::to_wstring(commandTargetFocusIsElsewhere)
				+ L", focusPreserved="
				+ std::to_wstring(explicitButtonIgnoredFocus)
				+ L", pageActivated="
				+ std::to_wstring(commandTargetPageActivated)
				+ L", requery="
				+ std::to_wstring(commandTargetRequeryInvalidated)
				+ L", page/focus restored="
				+ std::to_wstring(commandTargetPageRestored) + L"/"
				+ std::to_wstring(commandTargetFocusRestored)
				+ L", parameter=" + explicitButtonParameter
				+ L", canTarget=" + explicitButtonCanExecuteTarget
				+ L", executedTarget=" + explicitButtonExecutedTarget
				+ L", command=" + explicitButtonCommand
				+ L", traceAfterRequery=" + explicitButtonTraceAfterRequery
				+ L", traceAfterRestore=" + explicitButtonTraceAfterRestore
				+ L"]");

		resetCommandProbe();
		if (!openMenuItem->Invoke()
			|| _commandPreviewCanExecuteCount != 1
			|| _commandCanExecuteCount != 1
			|| _commandPreviewExecutedCount != 1
			|| _commandExecutedCount != 1
			|| _lastCommandParameter != L"menu-open"
			|| _commandRouteTrace != std::vector<std::wstring>{
				L"PreviewCanExecute:Demo.File.Open",
				L"CanExecute:Demo.File.Open",
				L"PreviewExecuted:Demo.File.Open",
				L"Executed:Demo.File.Open" }
			|| !_statusText
			|| _statusText->Text != L"RoutedCommand: Open · menu-open")
			return fail(L"Menu 命令未完成 PreviewCanExecute → CanExecute → PreviewExecuted → Executed。");

		resetCommandProbe();
		const auto controlO = Key::O;
		if (!cui::framework::InputAccess::ProcessCommandInput(
			*_basicButton, KeyEventArgs(
				controlO, ModifierKeys::Control))
			|| _lastCommandParameter != L"keyboard-open"
			|| _commandPreviewCanExecuteCount != 1
			|| _commandCanExecuteCount != 1
			|| _commandPreviewExecutedCount != 1
			|| _commandExecutedCount != 1)
			return fail(L"Window.KeyBinding Ctrl+O 未从焦点源路由到 RoutedCommand。");

		resetCommandProbe();
		if (!cui::framework::InputAccess::ProcessCommandInput(
			*_basicButton,
			MouseEventArgs(MouseButton::Middle, MouseButtonState::Pressed, 1, 0, 0, 0),
			ModifierKeys::Control)
			|| _lastCommandParameter != L"mouse-refresh"
			|| _commandCanExecuteCount != 1
			|| _commandExecutedCount != 1)
			return fail(L"Window.MouseBinding Ctrl+MiddleClick 未路由到 RoutedCommand。");

		auto* localCommandScope = dynamic_cast<Panel*>(
			FindGeneratedControlByName(L"wpfHierarchyScope"));
		auto* localCommandButton = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"wpfDispatcherProbe"));
		resetCommandProbe();
		const auto controlShiftP = Key::P;
		const auto localInputCount = localCommandScope
			? localCommandScope->GetInputBindings().size() : 0;
		const auto localButtonCommand = localCommandButton
			? static_cast<std::wstring>(localCommandButton->Command) : std::wstring{};
		const bool localKeyProcessed = localCommandScope && localCommandButton
			&& localInputCount == 2
			&& localButtonCommand == L"Demo.Wpf.Probe"
			&& localCommandButton->GetDisplayText()
				== L"Run Dispatcher/命令探针"
			&& localCommandButton->GetEffectiveAccessKey() == L'R'
			&& localCommandButton->GetAccessibilitySnapshot().KeyboardShortcut
				== L"Alt+R"
			&& cui::framework::InputAccess::ProcessCommandInput(
				*localCommandButton, KeyEventArgs(controlShiftP,
					ModifierKeys::Control | ModifierKeys::Shift));
		if (!localKeyProcessed
			|| _localCommandCanExecuteCount != 1
			|| _localCommandExecutedCount != 1
			|| _commandPreviewExecutedCount != 1
			|| _commandExecutedCount != 0
			|| _lastCommandParameter != L"local-keybinding"
			|| _commandRouteTrace != std::vector<std::wstring>{
				L"LocalCanExecute:Demo.Wpf.Probe",
				L"PreviewExecuted:Demo.Wpf.Probe",
				L"LocalExecuted:Demo.Wpf.Probe" })
		{
			std::wstring routeTrace;
			for (const auto& item : _commandRouteTrace)
			{
				if (!routeTrace.empty()) routeTrace += L"|";
				routeTrace += item;
			}
			return fail(L"控件级 CommandBinding/InputBinding 未在局部 route 闭环："
				L"scope=" + std::to_wstring(localCommandScope != nullptr)
				+ L", button=" + std::to_wstring(localCommandButton != nullptr)
				+ L", inputs=" + std::to_wstring(localInputCount)
				+ L", command=" + localButtonCommand
				+ L", processed=" + std::to_wstring(localKeyProcessed)
				+ L", display=" + (localCommandButton
					? localCommandButton->GetDisplayText() : std::wstring{})
				+ L", access=" + std::to_wstring(localCommandButton
					? static_cast<unsigned int>(
						localCommandButton->GetEffectiveAccessKey()) : 0u)
				+ L", shortcut=" + (localCommandButton
					? localCommandButton->GetAccessibilitySnapshot()
						.KeyboardShortcut : std::wstring{})
				+ L", focused=" + std::to_wstring(
					GetKeyboardFocusedElement() == localCommandButton)
				+ L", enabled=" + std::to_wstring(localCommandButton
					&& localCommandButton->IsEffectivelyEnabled())
				+ L", can/exec=" + std::to_wstring(_localCommandCanExecuteCount)
				+ L"/" + std::to_wstring(_localCommandExecutedCount)
				+ L", parameter=" + _lastCommandParameter
				+ L", trace=" + routeTrace + L"。");
		}

		resetCommandProbe();
		if (!cui::framework::InputAccess::ProcessCommandInput(
			*localCommandButton,
			MouseEventArgs(MouseButton::Right, MouseButtonState::Pressed, 1, 0, 0, 0),
			ModifierKeys::Alt)
			|| _localCommandCanExecuteCount != 1
			|| _localCommandExecutedCount != 1
			|| _commandPreviewExecutedCount != 1
			|| _commandExecutedCount != 0
			|| _lastCommandParameter != L"local-mousebinding"
			|| _commandRouteTrace != std::vector<std::wstring>{
				L"LocalCanExecute:Demo.Wpf.Probe",
				L"PreviewExecuted:Demo.Wpf.Probe",
				L"LocalExecuted:Demo.Wpf.Probe" })
			return fail(L"控件级 MouseBinding 未在局部 route 闭环。");

		auto* contextMore = _systemContextMenu
			? _systemContextMenu->GetItem(3) : nullptr;
		auto* refreshContextItem = _systemContextMenu
			? _systemContextMenu->GetItem(1) : nullptr;
		auto* copyContextItem = contextMore
			? contextMore->GetSubItem(0) : nullptr;
		if (!_systemContextMenu || !_systemContextMenu->GetItem(0)
			|| _systemContextMenu->GetItem(0)->Command
				!= L"Demo.System.NewProject"
			|| !refreshContextItem
			|| refreshContextItem->Command != L"Demo.System.Refresh"
			|| refreshContextItem->InputGestureText != L"F5"
			|| !refreshContextItem->HasAuthoredCommandTarget()
			|| refreshContextItem->CommandTarget != _menu
			|| refreshContextItem->GetPropertyValueSource(
				MenuItem::CommandTargetProperty())
				!= DependencyPropertyValueSource::Local
			|| dynamic_cast<Separator*>(
				_systemContextMenu->GetGeneratedItem(2)) == nullptr
			|| !contextMore || contextMore->ItemCount() != 2
			|| !copyContextItem
			|| !copyContextItem->HasAuthoredCommandTarget()
			|| copyContextItem->CommandTarget != systemSurface
			|| copyContextItem->GetPropertyValueSource(
				MenuItem::CommandTargetProperty())
				!= DependencyPropertyValueSource::Local
			|| !contextMore->GetSubItem(1)
			|| contextMore->GetSubItem(1)->Command != L"Demo.System.About"
			|| _systemContextMenu->GetItem(4) != nullptr)
			return fail(L"ContextMenu.Items 未从 XAML 完整物化层级与分隔符。");
		resetCommandProbe();
		_systemContextMenu->ShowAt(_basicButton, 0, 0);
		const bool contextMenuOpened = _systemContextMenu->IsOpen;
		const bool contextPlacementTargetShared =
			_systemContextMenu->PlacementTarget == _basicButton;
		const auto contextPlacementTrace = _commandRouteTrace;
		const int contextPlacementCanExecuteCount = _commandCanExecuteCount;
		resetCommandProbe();
		const bool contextCommandInvoked = refreshContextItem->Invoke();
		const int contextInvokePreviewCanExecuteCount =
			_commandPreviewCanExecuteCount;
		const int contextInvokeCanExecuteCount = _commandCanExecuteCount;
		const int contextInvokePreviewExecutedCount =
			_commandPreviewExecutedCount;
		const int contextInvokeExecutedCount = _commandExecutedCount;
		const auto contextInvokeParameter = _lastCommandParameter;
		const auto contextInvokeTrace = _commandRouteTrace;
		const auto contextInvokeStatus = _statusText
			? _statusText->Text : std::wstring{};
		const auto contextInvokeCanExecuteTarget =
			_lastCommandCanExecuteTarget;
		const auto contextInvokeExecutedTarget =
			_lastCommandExecutedTarget;
		const auto contextInvokeTargetText = commandTargetTrace->Text;
		resetCommandProbe();
		// Exercise the real keyboard route and the bottom-right placement used by
		// the System page instead of proving the nested state only by assigning it.
		_systemContextMenu->ShowAt(this, 1000, 650);
		const auto contextFocusBeforeNested = GetKeyboardFocusedElement();
		SetKeyboardFocus(contextMore, false);
		const bool nestedContextMoreFocused =
			GetKeyboardFocusedElement() == contextMore;
		const bool nestedContextInputHandled =
			cui::framework::InputAccess::DispatchInput(
				*this, KeyInput(InputReportKind::KeyDown, Key::Right));
		(void)cui::framework::InputAccess::DispatchInput(
			*this, KeyInput(InputReportKind::KeyUp, Key::Right));
		auto* nestedContextPopup = dynamic_cast<Popup*>(
			contextMore->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_Popup")));
		const auto nestedContextPopupRect = nestedContextPopup
			? nestedContextPopup->GetRenderedAbsoluteRectDip()
			: D2D1_RECT_F{};
		const auto contextViewport = GetContentViewportSizeDip();
		const bool nestedContextPopupInViewport = nestedContextPopup
			&& nestedContextPopupRect.left >= 0.0f
			&& nestedContextPopupRect.top >= 0.0f
			&& nestedContextPopupRect.right > nestedContextPopupRect.left
			&& nestedContextPopupRect.bottom > nestedContextPopupRect.top
			&& nestedContextPopupRect.right <= contextViewport.width
			&& nestedContextPopupRect.bottom <= contextViewport.height;
		const bool nestedContextPopupPresented =
			nestedContextMoreFocused && nestedContextInputHandled
			&& contextMore->IsSubmenuOpen
			&& nestedContextPopup && nestedContextPopup->IsOpen
			&& nestedContextPopupInViewport
			&& cui::framework::WindowAccess::
				GetTransientPresentationCount(*this) == 2;
		const bool nestedContextCommandInvoked = copyContextItem->Invoke();
		const auto nestedContextCanExecuteTarget =
			_lastCommandCanExecuteTarget;
		const auto nestedContextExecutedTarget =
			_lastCommandExecutedTarget;
		const auto nestedContextParameter = _lastCommandParameter;
		const auto nestedContextTargetText = commandTargetTrace->Text;
		_systemContextMenu->Hide();
		SetKeyboardFocus(contextFocusBeforeNested, false);
		const auto nestedContextTargetTextAfterHide = commandTargetTrace->Text;
		const bool contextTargetsSurvivedHide =
			_systemContextMenu->PlacementTarget == nullptr
			&& refreshContextItem->HasAuthoredCommandTarget()
			&& refreshContextItem->CommandTarget == _menu
			&& copyContextItem->HasAuthoredCommandTarget()
			&& copyContextItem->CommandTarget == systemSurface;
		const auto placementSaw = [&](const std::wstring& entry)
		{
			return std::find(
				contextPlacementTrace.begin(),
				contextPlacementTrace.end(),
				entry) != contextPlacementTrace.end();
		};
		if (!contextMenuOpened
			|| !contextPlacementTargetShared
			|| contextPlacementCanExecuteCount < 4
			|| !placementSaw(L"CanExecute:Demo.System.NewProject")
			|| !placementSaw(L"CanExecute:Demo.System.Refresh")
			|| !placementSaw(L"CanExecute:Demo.System.CopyInfo")
			|| !placementSaw(L"CanExecute:Demo.System.About")
			|| !contextCommandInvoked
			|| contextInvokeCanExecuteCount != 1
			|| contextInvokeExecutedCount != 1
			|| contextInvokePreviewCanExecuteCount != 0
			|| contextInvokePreviewExecutedCount != 1
			|| contextInvokeParameter != L"context-refresh"
			|| contextInvokeCanExecuteTarget != L"mainMenu"
			|| contextInvokeExecutedTarget != L"mainMenu"
			|| contextInvokeTargetText
				!= L"CanExecute target=mainMenu · Executed target=mainMenu · "
					L"Demo.System.Refresh"
			|| contextInvokeTrace != std::vector<std::wstring>{
				L"CanExecute:Demo.System.Refresh",
				L"PreviewExecuted:Demo.System.Refresh",
				L"Executed:Demo.System.Refresh" }
			|| contextInvokeStatus
				!= L"ContextMenu RoutedCommand: Refresh · context-refresh"
			|| !nestedContextPopupPresented
			|| !nestedContextCommandInvoked
			|| nestedContextCanExecuteTarget != L"systemSurface"
			|| nestedContextExecutedTarget != L"systemSurface"
			|| nestedContextParameter != L"context-copy"
			|| nestedContextTargetText
				!= L"CanExecute target=systemSurface · Executed target=systemSurface · "
					L"Demo.System.CopyInfo"
			|| nestedContextTargetTextAfterHide != nestedContextTargetText
			|| !contextTargetsSurvivedHide)
		{
			auto joinTrace = [](const std::vector<std::wstring>& trace)
			{
				std::wstring result;
				for (const auto& item : trace)
				{
					if (!result.empty()) result += L"|";
					result += item;
				}
				return result;
			};
			return fail(L"ContextMenu PlacementTarget 与显式/嵌套 CommandTarget "
				L"未按 XAML 分离："
				L"opened=" + std::to_wstring(contextMenuOpened)
				+ L", invoked=" + std::to_wstring(contextCommandInvoked)
				+ L", placement=" + std::to_wstring(contextPlacementTargetShared)
				+ L", enabled=" + std::to_wstring(
					refreshContextItem->IsEffectivelyEnabled())
				+ L", placementCan="
				+ std::to_wstring(contextPlacementCanExecuteCount)
				+ L", placementTrace=" + joinTrace(contextPlacementTrace)
				+ L", invoke previewCan/can/previewExec/exec="
				+ std::to_wstring(contextInvokePreviewCanExecuteCount) + L"/"
				+ std::to_wstring(contextInvokeCanExecuteCount) + L"/"
				+ std::to_wstring(contextInvokePreviewExecutedCount) + L"/"
				+ std::to_wstring(contextInvokeExecutedCount)
				+ L", parameter=" + contextInvokeParameter
				+ L", invokeTrace=" + joinTrace(contextInvokeTrace)
				+ L", refreshTarget=" + contextInvokeCanExecuteTarget + L"/"
				+ contextInvokeExecutedTarget
				+ L", nestedTarget=" + nestedContextCanExecuteTarget + L"/"
				+ nestedContextExecutedTarget
				+ L", nestedPopup="
				+ std::to_wstring(nestedContextPopupPresented)
				+ L", nestedFocused/input/inViewport="
				+ std::to_wstring(nestedContextMoreFocused) + L"/"
				+ std::to_wstring(nestedContextInputHandled) + L"/"
				+ std::to_wstring(nestedContextPopupInViewport)
				+ L", nestedRect="
				+ std::to_wstring(nestedContextPopupRect.left) + L","
				+ std::to_wstring(nestedContextPopupRect.top) + L","
				+ std::to_wstring(nestedContextPopupRect.right) + L","
				+ std::to_wstring(nestedContextPopupRect.bottom)
				+ L", targetAfterHide=" + nestedContextTargetTextAfterHide
				+ L", targetsSurvivedHide="
				+ std::to_wstring(contextTargetsSurvivedHide)
				+ L", status=" + contextInvokeStatus
				+ L"。");
		}
		if (!copyContextItem || !copyContextItem->IsLocallyEnabled()
			|| !copyContextItem->IsEffectivelyEnabled())
			return fail(L"MenuItem 初始本地/有效 IsEnabled 状态无效。");
		_copyInfoCommandEnabled = false;
		(void)RoutedCommandManager::InvalidateRequerySuggested(
			*_systemContextMenu);
		cui::PumpUIThreadCallbacks();
		if (!copyContextItem->IsLocallyEnabled()
			|| copyContextItem->IsEffectivelyEnabled()
			|| copyContextItem->Invoke())
			return fail(L"MenuItem 未把 CanExecute=false 投影为只读有效禁用状态。");
		copyContextItem->IsEnabled = false;
		_copyInfoCommandEnabled = true;
		(void)RoutedCommandManager::InvalidateRequerySuggested(
			*_systemContextMenu);
		cui::PumpUIThreadCallbacks();
		if (copyContextItem->IsLocallyEnabled()
			|| copyContextItem->IsEffectivelyEnabled()
			|| copyContextItem->Invoke())
			return fail(L"MenuItem requery 覆盖了作者声明的本地 IsEnabled=false。");
		copyContextItem->IsEnabled = true;
		(void)RoutedCommandManager::InvalidateRequerySuggested(
			*_systemContextMenu);
		cui::PumpUIThreadCallbacks();
		if (!copyContextItem->IsLocallyEnabled()
			|| !copyContextItem->IsEffectivelyEnabled())
			return fail(L"MenuItem 本地 IsEnabled 恢复后未显露 CanExecute=true。");
		resetCommandProbe();
		int requeryCount = 0;
		std::uint64_t lastRequeryGeneration = 0;
		auto requeryConnection =
			RoutedCommandManager::SubscribeRequerySuggested(*_menu,
				[&](const RoutedCommandRequeryEventArgs& args)
				{
					++requeryCount;
					lastRequeryGeneration = args.Generation;
				});
		const auto requeryBefore =
			RoutedCommandManager::GetRequeryGeneration(*_menu);
		const bool requeryInvalidated =
			RoutedCommandManager::InvalidateRequerySuggested(*_menu);
		cui::PumpUIThreadCallbacks();
		if (!requeryConnection.Connected() || !requeryInvalidated
			|| requeryCount != 1 || lastRequeryGeneration <= requeryBefore
			|| lastRequeryGeneration
				!= RoutedCommandManager::GetRequeryGeneration(*_menu))
			return fail(L"RoutedCommand Window-scope requery 通知不可观察。");

		auto* toolBasic = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"toolBasic"));
		auto* toolData = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"toolData"));
		auto* toolAnalytics = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"toolAnalytics"));
		auto* toolAnimation = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"toolAnimation"));
		auto* toolSystem = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"toolSystem"));
		auto* toolSeparator =
			FindGeneratedControlByName(L"toolSeparator");
		auto* toolIcon1 = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"toolIcon1"));
		auto* toolIcon2 = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"toolIcon2"));
		auto* toolIcon3 = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"toolIcon3"));
		auto* toolIconImage1 = dynamic_cast<Image*>(
			FindGeneratedControlByName(L"toolIconImage1"));
		auto* toolIconImage2 = dynamic_cast<Image*>(
			FindGeneratedControlByName(L"toolIconImage2"));
		auto* toolIconImage3 = dynamic_cast<Image*>(
			FindGeneratedControlByName(L"toolIconImage3"));
		if (!applyTemplateForVerification(_toolBar, L"mainToolBar"))
			return false;
		auto* toolTemplateRoot = _toolBar
			? cui::framework::TemplateAccess::GetTemplateRoot(*_toolBar)
			: nullptr;
		auto* toolChrome = _toolBar
			? _toolBar->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_ToolBarChrome")) : nullptr;
		auto* toolItemsPresenter = _toolBar
			? dynamic_cast<ItemsPresenter*>(
				_toolBar->FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"PART_ItemsPresenter"))) : nullptr;
		auto* toolItemsHost = _toolBar
			? dynamic_cast<StackPanel*>(
				cui::framework::TemplateAccess::GetItemsHost(*_toolBar)) : nullptr;
		const std::array<Control*, 9> authoredToolItems{
			toolBasic, toolData, toolAnalytics, toolAnimation, toolSystem, toolSeparator,
			toolIcon1, toolIcon2, toolIcon3 };
		bool authoredToolOwnershipValid = _toolBar && toolItemsHost;
		if (authoredToolOwnershipValid)
		{
			for (size_t index = 0; index < authoredToolItems.size(); ++index)
			{
				auto* item = authoredToolItems[index];
				if (!item || _toolBar->GetAuthoredItem(index) != item
					|| item->GetLogicalParent() != _toolBar
					|| item->GetVisualParent() != toolItemsHost)
				{
					authoredToolOwnershipValid = false;
					break;
				}
			}
		}
		if (!_toolBar || _toolBar->VisualChildCount() != 1
			|| _toolBar->GetVisualChild(0) != toolTemplateRoot
			|| toolTemplateRoot != toolChrome
			|| !toolItemsPresenter
			|| toolItemsPresenter->GetVisualParent() != toolChrome
			|| !toolItemsHost
			|| toolItemsHost->GetVisualParent() != toolItemsPresenter
			|| _toolBar->AuthoredItemCount() != authoredToolItems.size()
			|| toolItemsHost->VisualChildCount()
				!= static_cast<int>(authoredToolItems.size())
			|| toolItemsHost->GetOrientation() != Orientation::Horizontal
			|| !authoredToolOwnershipValid
			|| !toolIcon1 || !toolIcon2 || !toolIcon3
			|| !toolIconImage1 || !toolIconImage2 || !toolIconImage3
			|| !toolIconImage1->Source || !toolIconImage2->Source
			|| !toolIconImage3->Source
			|| toolIconImage1->Stretch
				!= ::Stretch::Uniform
			|| toolIconImage2->Stretch
				!= ::Stretch::Uniform
			|| toolIconImage3->Stretch
				!= ::Stretch::Uniform)
			return fail(L"ToolBar ControlTemplate/ItemsPresenter/ItemsHost 与 authored "
				L"Items 所有权未按 WPF 语义物化。");
		resetCommandProbe();
		if (!toolIcon1->Invoke()
			|| toolIcon1->Command != L"Demo.File.Open"
			|| _lastCommandParameter != L"toolbar-open"
			|| _commandPreviewCanExecuteCount != 1
			|| _commandCanExecuteCount != 1
			|| _commandPreviewExecutedCount != 1
			|| _commandExecutedCount != 1)
			return fail(L"Button.Command/CommandParameter 未进入 Window.CommandBinding。");
		const auto previousPage = _tabs->SelectedIndex;
		if (!toolData->Invoke()
			|| _tabs->SelectedIndex != PageIndex(DemoPage::Data))
			return fail(L"XAML ToolBar 按钮命名事件未驱动页面导航。");
		auto* animationGalleryControl = dynamic_cast<
			DemoWindowGeneratedAnimationGallery*>(
				FindGeneratedControlByName(L"animationGallery"));
		if (!toolAnimation || !animationGalleryControl
			|| !toolAnimation->Invoke()
			|| _tabs->SelectedIndex != PageIndex(DemoPage::Animation))
			return fail(L"动画画廊或 ToolBar 动画导航未生成。");
		const auto animationTick = ::GetTickCount64();
		if (!animationGalleryControl->RaisePlayAnimations()
			|| !animationGalleryControl->HasActiveVisualStateAnimations()
			|| !cui::framework::PresentationAccess::AdvanceVisualStateAnimations(
				*animationGalleryControl, animationTick + 2180)
			|| animationGalleryControl->GetCounter() < 40
			|| animationGalleryControl->GetCounter() > 80
			|| animationGalleryControl->GetLongCounter() <= 9007199254740993LL
			|| animationGalleryControl->GetModeText() != L"Half way")
			return fail(L"动画画廊主 Storyboard 未驱动类型化 Timeline/关键帧。");
		if (!animationGalleryControl->RaiseRemoveAnimations()
			|| !cui::framework::PresentationAccess::AdvanceVisualStateAnimations(
				*animationGalleryControl, animationTick + 2181)
			|| animationGalleryControl->HasActiveVisualStateAnimations()
			|| animationGalleryControl->GetCounter() != 0
			|| animationGalleryControl->GetModeText() != L"Idle")
			return fail(L"动画画廊 RemoveStoryboard 未恢复基础值。");
		(void)_tabs->SelectItem(previousPage);

		auto* windowContent = dynamic_cast<Grid*>(
			FindGeneratedControlByName(L"windowContent"));
		auto* firstStatusItem = _statusBar
			? dynamic_cast<StatusBarItem*>(
				_statusBar->GetGeneratedItem(0)) : nullptr;
		if (!applyTemplateForVerification(_statusBar, L"mainStatusBar")
			|| !applyTemplateForVerification(
				firstStatusItem, L"first StatusBarItem"))
			return false;
		auto* statusChrome = _statusBar
			? _statusBar->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_StatusBarChrome")) : nullptr;
		auto* statusItemsPresenter = _statusBar
			? dynamic_cast<ItemsPresenter*>(
				_statusBar->FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"PART_ItemsPresenter"))) : nullptr;
		auto* statusItemChrome = firstStatusItem
			? firstStatusItem->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_StatusBarItemChrome")) : nullptr;
		auto* statusContentPresenter = firstStatusItem
			? dynamic_cast<ContentPresenter*>(
				firstStatusItem->FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"PART_ContentPresenter"))) : nullptr;
		RequestLayout();
		UpdateLayout();
		const auto contentViewport = GetContentViewportSizeDip();
		const auto statusBounds = _statusBar
			? _statusBar->GetAbsoluteBoundsDip() : D2D1_RECT_F{};
		const auto statusSize = _statusBar
			? _statusBar->GetActualSizeDip() : cui::core::Size{};
		const auto statusItemSize = firstStatusItem
			? firstStatusItem->GetActualSizeDip() : cui::core::Size{};
		if (!windowContent || !_statusBar || _statusBar->ItemCount() != 2
			|| _statusBar->GeneratedItemCount() != 2
			|| !firstStatusItem || !statusChrome || !statusItemsPresenter
			|| !statusItemChrome || !statusContentPresenter
			|| cui::framework::TemplateAccess::GetTemplateRoot(*_statusBar)
				!= statusChrome
			|| cui::framework::TemplateAccess::GetItemsPresenter(*_statusBar)
				!= statusItemsPresenter
			|| _statusBar->GetPropertyValueSource(Control::TemplateProperty())
				!= DependencyPropertyValueSource::Theme
			|| _statusBar->GetPropertyValueSource(
				ItemsControl::ItemsPanelProperty())
				!= DependencyPropertyValueSource::Theme
			|| firstStatusItem->GetPropertyValueSource(
				Control::TemplateProperty())
				!= DependencyPropertyValueSource::Theme
			|| statusSize.width <= 0.0f || statusSize.height < 25.0f
			|| statusItemSize.width <= 0.0f
			|| statusItemSize.height <= 0.0f
			|| statusBounds.top < 0.0f
			|| statusBounds.bottom > contentViewport.height + 0.01f
			|| statusBounds.bottom <= statusBounds.top
			|| GetStatusBarItemText(1) != L"DemoWindow.cui.xaml")
		{
			return fail(L"StatusBar Grid 底部布局、Generic.xaml 模板、"
				L"ItemsPresenter 或 StatusBarItem 未完整物化："
				L"window/status/items/generated="
				+ std::to_wstring(windowContent != nullptr) + L"/"
				+ std::to_wstring(_statusBar != nullptr) + L"/"
				+ std::to_wstring(_statusBar ? _statusBar->ItemCount() : 0)
				+ L"/" + std::to_wstring(
					_statusBar ? _statusBar->GeneratedItemCount() : 0)
				+ L"，parts="
				+ std::to_wstring(firstStatusItem != nullptr) + L"/"
				+ std::to_wstring(statusChrome != nullptr) + L"/"
				+ std::to_wstring(statusItemsPresenter != nullptr) + L"/"
				+ std::to_wstring(statusItemChrome != nullptr) + L"/"
				+ std::to_wstring(statusContentPresenter != nullptr)
				+ L"，roots="
				+ std::to_wstring(_statusBar
					&& cui::framework::TemplateAccess::
						GetTemplateRoot(*_statusBar) == statusChrome)
				+ L"/" + std::to_wstring(_statusBar
					&& cui::framework::TemplateAccess::
						GetItemsPresenter(*_statusBar)
							== statusItemsPresenter)
				+ L"，sources="
				+ std::to_wstring(_statusBar ? static_cast<int>(
					_statusBar->GetPropertyValueSource(
						Control::TemplateProperty())) : -1)
				+ L"/" + std::to_wstring(_statusBar ? static_cast<int>(
					_statusBar->GetPropertyValueSource(
						ItemsControl::ItemsPanelProperty())) : -1)
				+ L"/" + std::to_wstring(firstStatusItem ? static_cast<int>(
					firstStatusItem->GetPropertyValueSource(
						Control::TemplateProperty())) : -1)
				+ L"，statusSize="
				+ std::to_wstring(statusSize.width) + L"x"
				+ std::to_wstring(statusSize.height)
				+ L"，itemSize=" + std::to_wstring(statusItemSize.width)
				+ L"x" + std::to_wstring(statusItemSize.height)
				+ L"，bounds=(" + std::to_wstring(statusBounds.left)
				+ L"," + std::to_wstring(statusBounds.top) + L","
				+ std::to_wstring(statusBounds.right) + L","
				+ std::to_wstring(statusBounds.bottom) + L")"
				+ L"，viewport=" + std::to_wstring(contentViewport.width)
				+ L"x" + std::to_wstring(contentViewport.height)
				+ L"，text=" + GetStatusBarItemText(1) + L"。");
		}

		struct LayoutTabScope final
		{
			TabControl& Tabs;
			int SelectedIndex;
			~LayoutTabScope()
			{
				Tabs.SelectedIndex = SelectedIndex;
			}
		} restoreLayoutTab{ *_tabs, _tabs->SelectedIndex };
		if (!_tabs->SelectItem(PageIndex(DemoPage::Layout)))
			return fail(L"无法切换到布局容器页执行可见布局验证。");
		RequestLayout();
		UpdateLayout();

		auto* relativePanel = dynamic_cast<RelativePanel*>(
			FindGeneratedControlByName(L"demoRelative"));
		auto* relativeCenter = dynamic_cast<StackPanel*>(
			FindGeneratedControlByName(
				L"relativeCenter"));
		auto* naturalTextProbe = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"naturalTextProbe"));
		auto* wrappedTextProbe = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wrappedTextProbe"));
		auto* trimmedTextProbe = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"trimmedTextProbe"));
		auto* relativeCenterButton =
			FindGeneratedControlByName(
				L"relativeCenterButton");
		auto* relativeConstraints = relativePanel && relativeCenter
			? relativePanel->GetConstraints(relativeCenter) : nullptr;
		if (!relativeConstraints || !relativeConstraints->CenterHorizontal
			|| !relativeConstraints->CenterVertical
			|| !naturalTextProbe || !wrappedTextProbe || !trimmedTextProbe
			|| !relativeCenterButton
			|| !relativePanel->ClipToBounds
			|| !relativeCenter->ClipToBounds
			|| !naturalTextProbe->Width.IsAuto()
			|| !naturalTextProbe->Height.IsAuto()
			|| naturalTextProbe->GetDesiredSizeDip().width <= 0.0f
			|| naturalTextProbe->GetDesiredSizeDip().width >= 500.0f
			|| std::abs(naturalTextProbe->GetActualSizeDip().width
				- naturalTextProbe->GetDesiredSizeDip().width) > 0.01f
			|| wrappedTextProbe->TextWrapping != TextWrapping::Wrap
			|| wrappedTextProbe->GetDesiredSizeDip().width > 320.01f
			|| wrappedTextProbe->GetDesiredSizeDip().height
				<= naturalTextProbe->GetDesiredSizeDip().height
			|| trimmedTextProbe->TextTrimming
				!= TextTrimming::CharacterEllipsis
			|| trimmedTextProbe->GetDesiredSizeDip().width > 220.01f
			|| relativeCenterButton->GetActualLocationDip().y
				+ relativeCenterButton->GetActualSizeDip().height
				> relativeCenter->GetActualSizeDip().height + 0.01f)
		{
			const auto naturalDesired = naturalTextProbe
				? naturalTextProbe->GetDesiredSizeDip() : cui::core::Size{};
			const auto naturalActual = naturalTextProbe
				? naturalTextProbe->GetActualSizeDip() : cui::core::Size{};
			const auto wrappedDesired = wrappedTextProbe
				? wrappedTextProbe->GetDesiredSizeDip() : cui::core::Size{};
			const auto trimmedDesired = trimmedTextProbe
				? trimmedTextProbe->GetDesiredSizeDip() : cui::core::Size{};
			const auto buttonLocation = relativeCenterButton
				? relativeCenterButton->GetActualLocationDip()
				: cui::core::Point{};
			const auto buttonSize = relativeCenterButton
				? relativeCenterButton->GetActualSizeDip() : cui::core::Size{};
			const auto centerSize = relativeCenter
				? relativeCenter->GetActualSizeDip() : cui::core::Size{};
			return fail(L"RelativePanel 约束或 TextBlock 的 Auto DesiredSize、"
				L"换行、省略、ClipToBounds 或父级高度贡献语义不完整："
				L"selected=" + std::to_wstring(_tabs->SelectedIndex)
				+ L"，constraints="
				+ std::to_wstring(relativeConstraints != nullptr) + L"/"
				+ std::to_wstring(relativeConstraints
					&& relativeConstraints->CenterHorizontal) + L"/"
				+ std::to_wstring(relativeConstraints
					&& relativeConstraints->CenterVertical)
				+ L"，parts=" + std::to_wstring(relativePanel != nullptr)
				+ L"/" + std::to_wstring(relativeCenter != nullptr) + L"/"
				+ std::to_wstring(naturalTextProbe != nullptr) + L"/"
				+ std::to_wstring(wrappedTextProbe != nullptr) + L"/"
				+ std::to_wstring(trimmedTextProbe != nullptr) + L"/"
				+ std::to_wstring(relativeCenterButton != nullptr)
				+ L"，clip=" + std::to_wstring(
					relativePanel && relativePanel->ClipToBounds) + L"/"
				+ std::to_wstring(
					relativeCenter && relativeCenter->ClipToBounds)
				+ L"，auto=" + std::to_wstring(
					naturalTextProbe && naturalTextProbe->Width.IsAuto())
				+ L"/" + std::to_wstring(
					naturalTextProbe && naturalTextProbe->Height.IsAuto())
				+ L"，natural desired/actual="
				+ std::to_wstring(naturalDesired.width) + L"x"
				+ std::to_wstring(naturalDesired.height) + L"/"
				+ std::to_wstring(naturalActual.width) + L"x"
				+ std::to_wstring(naturalActual.height)
				+ L"，wrapped kind/desired="
				+ std::to_wstring(wrappedTextProbe ? static_cast<int>(
					wrappedTextProbe->TextWrapping) : -1) + L"/"
				+ std::to_wstring(wrappedDesired.width) + L"x"
				+ std::to_wstring(wrappedDesired.height)
				+ L"，trim kind/desired="
				+ std::to_wstring(trimmedTextProbe ? static_cast<int>(
					trimmedTextProbe->TextTrimming) : -1) + L"/"
				+ std::to_wstring(trimmedDesired.width) + L"x"
				+ std::to_wstring(trimmedDesired.height)
				+ L"，button=" + std::to_wstring(buttonLocation.y)
				+ L"+" + std::to_wstring(buttonSize.height)
				+ L"，center=" + std::to_wstring(centerSize.width) + L"x"
				+ std::to_wstring(centerSize.height) + L"。");
		}

		auto* layoutSurface = dynamic_cast<Grid*>(
			FindGeneratedControlByName(L"layoutSurface"));
		auto* canvasProbe = dynamic_cast<Canvas*>(
			FindGeneratedControlByName(L"canvasSemanticsProbe"));
		auto* canvasLeftWins = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"canvasLeftWins"));
		auto* canvasRightBottom = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"canvasRightBottom"));
		std::wstring canvasTag;
		if (!layoutSurface || !canvasProbe || !canvasLeftWins || !canvasRightBottom
			|| layoutSurface->GetRows().size() != 3
			|| layoutSurface->GetColumns().size() != 4
			|| !layoutSurface->GetRows()[0].Height.IsAuto()
			|| !layoutSurface->GetRows()[2].Height.IsStar()
			|| Grid::GetRow(*canvasProbe) != 0
			|| Grid::GetColumn(*canvasProbe) != 3
			|| canvasProbe->Type() != UIClass::UI_Canvas
			|| canvasProbe->Cursor != CursorKind::Cross
			|| canvasProbe->GetPropertyValueSource(Control::CursorProperty())
				!= DependencyPropertyValueSource::Local
			|| canvasLeftWins->Cursor != CursorKind::Cross
			|| canvasLeftWins->GetPropertyValueSource(Control::CursorProperty())
				!= DependencyPropertyValueSource::Inherited
			|| canvasLeftWins->ResolvePointerCursor(1, 1) != CursorKind::Cross
			|| !canvasProbe->Tag.TryGetString(canvasTag)
			|| canvasTag != L"cursor-inheritance-root"
			|| canvasProbe->GetPropertyValueSource(Control::TagProperty())
				!= DependencyPropertyValueSource::Local
			|| !std::isnan(Canvas::GetLeft(*canvasProbe))
			|| !std::isnan(Canvas::GetTop(*canvasProbe))
			|| !std::isnan(Canvas::GetRight(*canvasProbe))
			|| !std::isnan(Canvas::GetBottom(*canvasProbe))
			|| std::abs(Canvas::GetLeft(*canvasLeftWins) - 0.25f) > 0.001f
			|| std::abs(Canvas::GetRight(*canvasLeftWins) - 40.0f) > 0.001f
			|| std::abs(Canvas::GetRight(*canvasRightBottom) - 0.75f) > 0.001f
			|| std::abs(Canvas::GetBottom(*canvasRightBottom) - 0.5f) > 0.001f)
			return fail(L"Grid 页面骨架或隔离的 Canvas 语义探针未从 XAML 正确物化。");
		canvasProbe->UpdateLayout();
		const auto probeSize = canvasProbe->GetActualSizeDip();
		const auto rightBottomSize = canvasRightBottom->GetActualSizeDip();
		const auto expectedRightBottomX = probeSize.width
			- Canvas::GetRight(*canvasRightBottom) - rightBottomSize.width
			- canvasRightBottom->Margin.Right;
		const auto expectedRightBottomY = probeSize.height
			- Canvas::GetBottom(*canvasRightBottom) - rightBottomSize.height
			- canvasRightBottom->Margin.Bottom;
		const auto leftWinsLocation = canvasLeftWins->GetActualLocationDip();
		const auto rightBottomLocation = canvasRightBottom->GetActualLocationDip();
		if (std::abs(leftWinsLocation.x - 1.25f) > 0.001f
			|| std::abs(leftWinsLocation.y - 2.5f) > 0.001f
			|| std::abs(rightBottomLocation.x - expectedRightBottomX) > 0.001f
			|| std::abs(rightBottomLocation.y - expectedRightBottomY) > 0.001f)
			return fail(L"Canvas Left/Top 优先级、Right/Bottom 回退或 Margin 布局不正确。"
				L" left=" + std::to_wstring(leftWinsLocation.x) + L","
				+ std::to_wstring(leftWinsLocation.y) + L" expected=1.25,2.5; right="
				+ std::to_wstring(rightBottomLocation.x) + L","
				+ std::to_wstring(rightBottomLocation.y) + L" expected="
				+ std::to_wstring(expectedRightBottomX) + L","
				+ std::to_wstring(expectedRightBottomY));

		struct WpfTabScope final
		{
			TabControl& Tabs;
			int SelectedIndex;
			~WpfTabScope()
			{
				Tabs.SelectedIndex = SelectedIndex;
			}
		} restoreWpfTab{ *_tabs, _tabs->SelectedIndex };
		if (!_tabs->SelectItem(PageIndex(DemoPage::WpfSemantics)))
			return fail(L"无法切换到 WPF 语义实验页执行可见输入验证。");
		RequestLayout();
		UpdateLayout();

		auto* wpfSurface = FindGeneratedControlByName(
			L"wpfLabSurface");
		auto* typographyScope = FindGeneratedControlByName(
			L"wpfBindingScope");
		auto* typographyOverride = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfTypographyOverride"));
		auto* editor = dynamic_cast<TextBox*>(
			FindGeneratedControlByName(L"wpfTwoWayEditor"));
		auto* elementMirror = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfElementMirror"));
		auto* selfValue = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfSelfValue"));
		auto* ancestorValue = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfAncestorValue"));
		auto* fallbackValue = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfFallbackValue"));
		auto* nullValue = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfNullValue"));
		auto* indexerValue = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfIndexerValue"));
		auto* keyedIndexerValue = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfKeyedIndexerValue"));
		auto* convertedValue = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfConvertedValue"));
		auto* multiValue = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfMultiValue"));
		if (!wpfSurface || !typographyScope || !typographyOverride
			|| !editor || !elementMirror || !selfValue
			|| !ancestorValue || !fallbackValue || !nullValue || !indexerValue
			|| !keyedIndexerValue || !convertedValue || !multiValue)
			return fail(L"WPF 语义实验区的 XAML 控件树未完整物化。");
		if (editor->SelectionBrush.Kind != cui::drawing::BrushKind::Solid
			|| std::abs(editor->SelectionBrush.Color.r - 0.4862745f) > 0.001f
			|| std::abs(editor->SelectionBrush.Color.g - 0.2274510f) > 0.001f
			|| std::abs(editor->SelectionBrush.Color.b - 0.9294118f) > 0.001f
			|| std::abs(editor->SelectionOpacity - 0.45) > 0.001
			|| editor->SelectionTextBrush.Kind
				!= cui::drawing::BrushKind::Solid
			|| editor->SelectionTextBrush.Color.r < 0.999f
			|| editor->CaretBrush.Kind != cui::drawing::BrushKind::Solid
			|| std::abs(editor->CaretBrush.Color.r - 0.4862745f) > 0.001f)
			return fail(L"TextBoxBase SelectionBrush/SelectionOpacity/"
				L"SelectionTextBrush/CaretBrush 未按 XAML 物化：selectionKind="
				+ std::to_wstring(static_cast<int>(editor->SelectionBrush.Kind))
				+ L"，selection=" + std::to_wstring(
					editor->SelectionBrush.Color.r) + L","
				+ std::to_wstring(editor->SelectionBrush.Color.g) + L","
				+ std::to_wstring(editor->SelectionBrush.Color.b)
				+ L"，opacity=" + std::to_wstring(editor->SelectionOpacity)
				+ L"，textKind=" + std::to_wstring(static_cast<int>(
					editor->SelectionTextBrush.Kind))
				+ L"，textR=" + std::to_wstring(
					editor->SelectionTextBrush.Color.r)
				+ L"，caretKind=" + std::to_wstring(static_cast<int>(
					editor->CaretBrush.Kind))
				+ L"，caretR=" + std::to_wstring(
					editor->CaretBrush.Color.r));
		if (GetPropertyValueSource(Control::DataContextProperty())
				!= DependencyPropertyValueSource::Local
			|| GetDataContext().Get() != _dataContext.get()
			|| typographyScope->GetPropertyValueSource(
				Control::DataContextProperty())
				!= DependencyPropertyValueSource::Inherited
			|| typographyScope->GetDataContext().Get() != _dataContext.get()
			|| nullValue->GetDataContext().Get() != _dataContext.get()
			|| indexerValue->GetDataContext().Get() != _dataContext.get())
			return fail(L"Window.DataContext 未作为真实 Content 继承边界传播。");
		if (typographyScope->GetPropertyValueSource(Control::FontFamilyProperty())
				!= DependencyPropertyValueSource::Local
			|| typographyScope->GetPropertyExpressionKind(
				Control::FontFamilyProperty(),
				DependencyPropertyValueSource::Local)
				!= DependencyPropertyExpressionKind::DynamicResource
			|| typographyScope->GetPropertyExpressionKind(
				Control::FontSizeProperty(),
				DependencyPropertyValueSource::Local)
				!= DependencyPropertyExpressionKind::DynamicResource
			|| elementMirror->GetPropertyValueSource(Control::FontFamilyProperty())
				!= DependencyPropertyValueSource::Inherited
			|| elementMirror->GetPropertyValueSource(Control::FontSizeProperty())
				!= DependencyPropertyValueSource::Inherited
			|| elementMirror->FontFamily != L"Consolas"
			|| std::abs(elementMirror->FontSize - 15.0f) > 0.001f
			|| typographyOverride->GetPropertyValueSource(
				Control::FontSizeProperty())
				!= DependencyPropertyValueSource::Local
			|| std::abs(typographyOverride->FontSize - 14.0f) > 0.001f)
			return fail(L"Typography 继承、Local 覆盖或 DynamicResource 表达式未闭环。");
		auto* editorBinding = editor->DataBindings.Find(TextBox::TextProperty());
		if (!editorBinding || editorBinding->Mode() != BindingMode::TwoWay
			|| editorBinding->UpdateMode()
				!= DataSourceUpdateMode::OnPropertyChanged
			|| editor->GetPropertyValueSource(TextBox::TextProperty())
				!= DependencyPropertyValueSource::Local
			|| editor->GetPropertyExpressionKind(TextBox::TextProperty())
				!= DependencyPropertyExpressionKind::Binding
			|| editor->Text != L"Ada" || elementMirror->Text != L"Ada"
			|| selfValue->AutomationName != L"RelativeSource Self"
			|| ancestorValue->AutomationName != L"FindAncestor · StackPanel")
			return fail(L"TwoWay / ElementName / RelativeSource 源解析语义未闭环。");
		if (fallbackValue->Text != L"Fallback: unavailable"
			|| nullValue->Text != L"TargetNull: (none)"
			|| indexerValue->Text != L"Ada"
			|| keyedIndexerValue->Text != L"settings[indexer]"
			|| convertedValue->Text != L"trimmed: WPF runtime"
			|| multiValue->Text != L"Ada / Lovelace / runtime data")
			return fail(L"Fallback/TargetNull/索引器/Converter/StringFormat/MultiBinding 未按 XAML 求值。"
				L" fallback='" + fallbackValue->Text + L"' null='"
				+ nullValue->Text + L"' index='" + indexerValue->Text
				+ L"' keyed='" + keyedIndexerValue->Text + L"' converted='"
				+ convertedValue->Text + L"' multi='" + multiValue->Text + L"'");
		if (!editor->TrySetCurrentPropertyValue(
			TextBox::TextProperty(),
			BindingValue(std::wstring(L"Augusta"))))
			return fail(L"TwoWay TextBox 无法更新绑定源。");
		BindingValue firstName;
		if (!_dataContext->TryGetValue(L"WpfFirst", firstName)
			|| firstName.ToString() != L"Augusta"
			|| editor->GetPropertyExpressionKind(TextBox::TextProperty())
				!= DependencyPropertyExpressionKind::Binding
			|| elementMirror->Text != L"Augusta"
			|| multiValue->Text != L"Augusta / Lovelace / runtime data")
			return fail(L"TwoWay 更新未传播到数据源、ElementName 与 MultiBinding。");
		if (!_dataContext->SetValue(L"WpfLast", std::wstring(L"Hopper"))
			|| multiValue->Text != L"Augusta / Hopper / runtime data"
			|| !_dataContext->SetValue(
				L"WpfNullable", std::wstring(L"available"))
			|| nullValue->Text != L"available"
			|| !_dataContext->TrySetValue(L"WpfNullable", BindingValue{})
			|| nullValue->Text != L"TargetNull: (none)")
			return fail(L"源到目标更新或 TargetNull 动态恢复未生效。");

		auto* templateButton = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"wpfTemplateButton"));
		if (!applyTemplateForVerification(
			templateButton, L"wpfTemplateButton"))
			return false;
		auto* templatedParentValue = templateButton
			? dynamic_cast<Label*>(templateButton->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"wpfTemplatedParentValue"))) : nullptr;
		auto* buttonPresenter = templateButton
			? dynamic_cast<ContentPresenter*>(
				templateButton->FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"wpfButtonContentPresenter"))) : nullptr;
		auto* buttonChrome = templateButton
			? templateButton->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"wpfButtonChrome"))
			: nullptr;
		auto* treeRelationValue = templateButton
			? dynamic_cast<Label*>(templateButton->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"wpfTreeRelationValue"))) : nullptr;
		auto* buttonTemplateLayout = templatedParentValue
			? templatedParentValue->GetVisualParent() : nullptr;
		if (!templateButton || !buttonChrome
			|| !templatedParentValue || !treeRelationValue || !buttonPresenter
			|| !buttonTemplateLayout || buttonTemplateLayout == buttonChrome
			|| cui::framework::TemplateAccess::GetContentPresenter(*templateButton)
				!= buttonPresenter
			|| buttonChrome->GetVisualParent() != templateButton
			|| buttonChrome->GetLogicalParent() != nullptr
			|| buttonChrome->GetTemplatedParent() != templateButton
			|| buttonChrome->GetRoutedParent() != templateButton
			|| buttonTemplateLayout->GetVisualParent() != buttonChrome
			|| buttonTemplateLayout->GetLogicalParent() != nullptr
			|| buttonTemplateLayout->GetTemplatedParent() != templateButton
			|| buttonTemplateLayout->GetInheritanceParent() != buttonChrome
			|| templatedParentValue->GetVisualParent() != buttonTemplateLayout
			|| templatedParentValue->GetLogicalParent() != nullptr
			|| templatedParentValue->GetTemplatedParent() != templateButton
			|| templatedParentValue->GetInheritanceParent()
				!= buttonTemplateLayout
			|| treeRelationValue->GetVisualParent() != buttonTemplateLayout
			|| treeRelationValue->GetLogicalParent() != nullptr
			|| treeRelationValue->GetTemplatedParent() != templateButton
			|| treeRelationValue->GetInheritanceParent()
				!= buttonTemplateLayout
			|| buttonPresenter->GetVisualParent() != buttonTemplateLayout
			|| buttonPresenter->GetLogicalParent() != nullptr
			|| buttonPresenter->GetTemplatedParent() != templateButton
			|| buttonPresenter->GetInheritanceParent() != buttonTemplateLayout
			|| templatedParentValue->Text != L"RelativeSource TemplatedParent"
			|| buttonChrome->GetPropertyValueSource(Border::PaddingProperty())
				!= DependencyPropertyValueSource::Template
			|| buttonChrome->GetPropertyExpressionKind(
				Border::PaddingProperty(),
				DependencyPropertyValueSource::Template)
				!= DependencyPropertyExpressionKind::None
			|| templatedParentValue->GetPropertyExpressionKind(
				Label::TextProperty())
				!= DependencyPropertyExpressionKind::Binding)
			return fail(L"ControlTemplate / ContentPresenter / TemplatedParent 未实际接管 Button。");

		const auto primaryButtonTemplate = templateButton->GetTemplate();
		const ControlWeakReference primaryButtonRoot(buttonChrome);
		BindingValue alternateButtonTemplateValue;
		ControlTemplateReference alternateButtonTemplate;
		if (!primaryButtonTemplate
			|| templateButton->GetPropertyValueSource(
				Control::TemplateProperty())
				!= DependencyPropertyValueSource::Local
			|| !templateButton->TryFindResource(
				L"WpfLabButtonTemplateAlternate",
				alternateButtonTemplateValue)
			|| !alternateButtonTemplateValue.TryGet(
				alternateButtonTemplate)
			|| !alternateButtonTemplate)
			return fail(L"Control.Template DP 或备用模板资源未发布。");
		templateButton->SetTemplate(alternateButtonTemplate);
		if (cui::framework::TemplateAccess::GetTemplateRoot(
				*templateButton) != nullptr
			|| primaryButtonRoot.Get() != nullptr
			|| !templateButton->ApplyTemplate())
			return fail(L"Control.Template 变更未拆除旧树或无法原位重建。");
		auto* alternateButtonChrome =
			templateButton->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"wpfAlternateButtonChrome"));
		auto* alternateButtonPresenter =
			dynamic_cast<ContentPresenter*>(
				templateButton->FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"wpfAlternateContentPresenter")));
		if (!alternateButtonChrome || !alternateButtonPresenter
			|| alternateButtonChrome
				!= cui::framework::TemplateAccess::GetTemplateRoot(
					*templateButton)
			|| alternateButtonChrome->GetVisualParent() != templateButton
			|| alternateButtonChrome->GetLogicalParent() != nullptr
			|| alternateButtonChrome->GetTemplatedParent() != templateButton
			|| alternateButtonChrome->GetInheritanceParent() != templateButton
			|| alternateButtonPresenter->GetTemplatedParent()
				!= templateButton
			|| alternateButtonPresenter->GetLogicalParent() != nullptr
			|| alternateButtonPresenter->GetInheritanceParent()
				!= alternateButtonPresenter->GetVisualParent()
			|| cui::framework::TemplateAccess::GetContentPresenter(
				*templateButton)
				!= alternateButtonPresenter
			|| alternateButtonPresenter->GetGeneratedContent() == nullptr)
			return fail(L"备用 ControlTemplate 的树关系或 ContentPresenter 未闭环。");
		const ControlWeakReference alternateButtonRoot(
			alternateButtonChrome);
		templateButton->SetTemplate(primaryButtonTemplate);
		if (alternateButtonRoot.Get() != nullptr
			|| !templateButton->ApplyTemplate()
			|| !templateButton->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"wpfButtonChrome"))
			|| templateButton->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"wpfAlternateButtonChrome"))
			|| !templateButton->LastTemplateError().empty())
			return fail(L"Control.Template 往返重套未恢复主模板。");

		auto* triggerButton = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"wpfTriggerButton"));
		auto* scopedResource = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfScopeResourceValue"));
		auto* innerResource = dynamic_cast<Label*>(
			FindGeneratedControlByName(L"wpfInnerResourceValue"));
		if (!triggerButton || !scopedResource || !innerResource
			|| triggerButton->GetPropertyValueSource(Control::BackgroundProperty())
				!= DependencyPropertyValueSource::Style
			|| triggerButton->GetPropertyExpressionKind(
				Control::BackgroundProperty(),
				DependencyPropertyValueSource::Style)
				!= DependencyPropertyExpressionKind::DynamicResource
			|| scopedResource->GetPropertyValueSource(Label::ForegroundProperty())
				!= DependencyPropertyValueSource::Local
			|| scopedResource->GetPropertyExpressionKind(
				Label::ForegroundProperty())
				!= DependencyPropertyExpressionKind::DynamicResource
			|| std::abs(triggerButton->FontSize - 13.0) > 0.001
			|| triggerButton->IsDefault
			|| std::abs(triggerButton->Background.Color.g - 0xA3 / 255.0f) > 0.001f
			|| std::abs(scopedResource->Foreground.Color.g - 0xA3 / 255.0f) > 0.001f
			|| std::abs(innerResource->Foreground.Color.r - 0xF0 / 255.0f) > 0.001f)
			return fail(L"BasedOn / DynamicResource 局部遮蔽的初始值不正确。");
		const auto localResources =
			cui::framework::StyleAccess::Resources(*wpfSurface);
		if (!localResources || !localResources->IsImmutable()
			|| std::abs(innerResource->Foreground.Color.r - 0xF0 / 255.0f) > 0.001f
			|| elementMirror->FontFamily != L"Consolas"
			|| std::abs(elementMirror->FontSize - 15.0f) > 0.001f
			|| std::abs(typographyOverride->FontSize - 14.0f) > 0.001f)
			return fail(L"AOT 词法资源未冻结为不可变程序或破坏局部覆盖与资源遮蔽。");
		if (!_dataContext->SetValue(L"WpfStatus", std::wstring(L"Ready"))
			|| !triggerButton->HasActiveVisualStateAnimations())
			return fail(L"DataTrigger EnterActions 未启动 Storyboard。");
		const auto stylePulseTick = ::GetTickCount64();
		if (!cui::framework::PresentationAccess::AdvanceVisualStateAnimations(*triggerButton, stylePulseTick + 120)
			|| triggerButton->FontSize <= 14.0 || triggerButton->FontSize >= 18.0
			|| !_dataContext->SetValue(L"WpfIsAdmin", true)
			|| !triggerButton->IsDefault)
			return fail(L"DataTrigger 动画或 MultiDataTrigger AND 条件未生效。");
		if (std::abs(
			triggerButton->BorderThickness.MaxEdge() - 4.0f) > 0.001f)
			return fail(L"MultiTrigger 属性/状态 AND 条件未生效。");
		if (!_dataContext->SetValue(L"WpfStatus", std::wstring(L"Idle"))
			|| !triggerButton->HasActiveVisualStateAnimations())
			return fail(L"DataTrigger ExitActions StopStoryboard 未保持 next-tick 控制语义。");
		if (!cui::framework::PresentationAccess::AdvanceVisualStateAnimations(
				*triggerButton, stylePulseTick + 121)
			|| triggerButton->HasActiveVisualStateAnimations()
			|| std::abs(triggerButton->FontSize - 13.0) > 0.001
			|| triggerButton->IsDefault)
			return fail(L"DataTrigger ExitActions StopStoryboard 未在下一 tick 回退样式值。");
		(void)_dataContext->SetValue(L"WpfIsAdmin", false);

		auto* templateList = dynamic_cast<ListBox*>(
			FindGeneratedControlByName(L"wpfTemplateList"));
		if (!applyTemplateForVerification(
			templateList, L"wpfTemplateList"))
			return false;
		auto* itemsPresenter = templateList
			? dynamic_cast<ItemsPresenter*>(
				templateList->FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"wpfItemsPresenter")))
			: nullptr;
		auto* firstContainer = templateList
			? dynamic_cast<ListBoxItem*>(templateList->GetGeneratedItem(0)) : nullptr;
		if (!applyTemplateForVerification(
			firstContainer, L"first wpfTemplateList item"))
			return false;
		auto* firstChrome = firstContainer
			? firstContainer->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"wpfItemChrome")) : nullptr;
		auto* firstPresenter = firstContainer
			? dynamic_cast<ContentPresenter*>(
				firstContainer->FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"wpfItemContentPresenter"))) : nullptr;
		auto* listTemplateRoot = templateList
			? cui::framework::TemplateAccess::GetTemplateRoot(*templateList)
			: nullptr;
		auto* listItemsHost = templateList
			? cui::framework::TemplateAccess::GetItemsHost(*templateList) : nullptr;
		if (!templateList || !itemsPresenter || !listTemplateRoot
			|| cui::framework::TemplateAccess::GetItemsPresenter(*templateList)
				!= itemsPresenter
			|| !listItemsHost
			|| cui::framework::TemplateAccess::GetItemsHost(*itemsPresenter)
				!= listItemsHost
			|| listTemplateRoot->GetVisualParent() != templateList
			|| listTemplateRoot->GetLogicalParent() != nullptr
			|| listTemplateRoot->GetTemplatedParent() != templateList
			|| itemsPresenter->GetVisualParent() != listTemplateRoot
			|| itemsPresenter->GetLogicalParent() != nullptr
			|| itemsPresenter->GetTemplatedParent() != templateList
			|| itemsPresenter->GetInheritanceParent() != listTemplateRoot
			|| listItemsHost->GetVisualParent() != itemsPresenter
			|| listItemsHost->GetLogicalParent() != nullptr
			|| listItemsHost->GetInheritanceParent() != itemsPresenter
			|| templateList->GeneratedItemCount() != 2
			|| !templateList->GetItemContainerTemplate()
			|| !firstContainer || !firstChrome || !firstPresenter
			|| firstContainer->GetVisualParent() != listItemsHost
			|| firstContainer->GetLogicalParent() != templateList
			|| firstChrome->GetVisualParent() != firstContainer
			|| firstChrome->GetLogicalParent() != nullptr
			|| firstChrome->GetTemplatedParent() != firstContainer
			|| firstChrome->GetInheritanceParent() != firstContainer
			|| firstPresenter->GetLogicalParent() != nullptr
			|| firstPresenter->GetInheritanceParent()
				!= firstPresenter->GetVisualParent()
			|| cui::framework::TemplateAccess::GetContentPresenter(*firstContainer)
				!= firstPresenter
			|| !firstContainer->Content())
			return fail(
				L"ItemsPresenter / ListBoxItem ControlTemplate / "
				L"ContentPresenter 未闭环：objects="
				+ std::to_wstring(templateList != nullptr)
				+ L"/" + std::to_wstring(itemsPresenter != nullptr)
				+ L"/" + std::to_wstring(listTemplateRoot != nullptr)
				+ L"/" + std::to_wstring(listItemsHost != nullptr)
				+ L"/" + std::to_wstring(firstContainer != nullptr)
				+ L"/" + std::to_wstring(firstChrome != nullptr)
				+ L"/" + std::to_wstring(firstPresenter != nullptr)
				+ L", registrations="
				+ std::to_wstring(templateList && itemsPresenter
					&& cui::framework::TemplateAccess::GetItemsPresenter(
						*templateList) == itemsPresenter)
				+ L"/" + std::to_wstring(
					templateList && itemsPresenter && listItemsHost
					&& cui::framework::TemplateAccess::GetItemsHost(
						*templateList) == listItemsHost
					&& cui::framework::TemplateAccess::GetItemsHost(
						*itemsPresenter) == listItemsHost)
				+ L"/" + std::to_wstring(firstContainer && firstPresenter
					&& cui::framework::TemplateAccess::GetContentPresenter(
						*firstContainer) == firstPresenter)
				+ L", itemState="
				+ std::to_wstring(templateList
					? templateList->GeneratedItemCount() : 0)
				+ L"/" + std::to_wstring(templateList
					&& static_cast<bool>(
						templateList->GetItemContainerTemplate()))
				+ L"/" + std::to_wstring(firstContainer
					&& static_cast<bool>(firstContainer->Content()))
				+ L", contentParents="
				+ std::to_wstring(firstPresenter
					&& firstPresenter->GetLogicalParent() == nullptr)
				+ L"/" + std::to_wstring(firstPresenter
					&& firstPresenter->GetInheritanceParent()
						== firstPresenter->GetVisualParent()));
		auto* selectedContainer = dynamic_cast<ListBoxItem*>(
			templateList->GetGeneratedItem(1));
		if (!applyTemplateForVerification(
			selectedContainer, L"selected wpfTemplateList item"))
			return false;
		if (!templateList->SelectIndex(1))
			return fail(L"XAML ListBoxItem 容器无法进入选择状态。");
		if (!selectedContainer
			|| selectedContainer->GetCurrentVisualState(
				MakeVisualStateGroupToken(L"SelectionStates"))
				!= MakeVisualStateToken(L"Selected"))
			return fail(L"ListBoxItem XAML VisualState 未响应选择。");

		auto* routedSource = FindGeneratedControlByName(
			L"wpfRouteSource");
		auto* focusOuter = FindGeneratedControlByName(
			L"wpfRouteOuter");
		auto* focusScope = FindGeneratedControlByName(
			L"wpfRouteMiddle");
		auto* focusPeerB = FindGeneratedControlByName(
			L"wpfFocusPeerB");
		auto* focusPeerC = FindGeneratedControlByName(
			L"wpfFocusPeerC");
		auto* noFocusPeer = FindGeneratedControlByName(
			L"wpfNoFocusPeer");
		auto* routedButton = dynamic_cast<Button*>(routedSource);
		auto* focusOuterBorder = dynamic_cast<Border*>(focusOuter);
		_routedInputTrace.clear();
		MouseEventArgs routedArgs(
			MouseButton::Left, MouseButtonState::Pressed,
			1, 8, 8, 0);
		if (!routedSource || !focusOuter || !focusScope
			|| !focusPeerB || !focusPeerC || !noFocusPeer
			|| !routedButton || !focusOuterBorder)
			return fail(L"XAML routed-input 演示源不存在。");
		routedSource->OnMouseDown(routedSource, routedArgs);
		const std::vector<std::wstring> expectedRoute{
			L"T outer", L"T middle", L"T source",
			L"B source(H)", L"B outer(too)" };
		if (_routedInputTrace != expectedRoute
			|| !routedArgs.Handled
			|| routedArgs.OriginalSource != routedSource
			|| routedArgs.Source != routedSource
			|| routedArgs.CurrentTarget != nullptr)
			return fail(L"内建输入事件未共享 tunnel/bubble、Handled 与 handledEventsToo 路由。");

		_routedInputTrace.clear();
		KeyEventArgs routedKey(
			Key::F10, ModifierKeys::Control | ModifierKeys::Shift);
		routedSource->OnKeyDown(routedSource, routedKey);
		const std::vector<std::wstring> expectedKeyRoute{
			L"K.T outer", L"K.T source", L"K.B source", L"K.B outer" };
		if (_routedInputTrace != expectedKeyRoute
			|| routedKey.Key != Key::F10
			|| routedKey.SystemKey != Key::None
			|| !routedKey.HasModifier(ModifierKeys::Control)
			|| !routedKey.HasModifier(ModifierKeys::Shift)
			|| routedKey.OriginalSource != routedSource
			|| _routedInputDetail != L"Key=Ctrl+Shift+F10 · first")
			return fail(L"Key/SystemKey/ModifierKeys 未作为独立 WPF 输入身份沿同一路由传播。");

		_routedInputTrace.clear();
		const bool captured = routedSource->CaptureMouse();
		const bool ownedAfterCapture = GetMouseCaptured() == routedSource;
		const auto captureTrace = _routedInputTrace;
		const bool released = routedSource->ReleaseMouseCapture();
		const bool clearAfterRelease = GetMouseCaptured() == nullptr;
		if (!captured || !ownedAfterCapture
			|| captureTrace != std::vector<std::wstring>{ L"capture+" }
			|| !released || !clearAfterRelease
			|| _routedInputTrace != std::vector<std::wstring>{
				L"capture+", L"capture-" })
			return fail(L"MouseCapture 未由 InputManager 原子获取、路由并释放：capture="
				+ std::to_wstring(captured) + L", owned="
				+ std::to_wstring(ownedAfterCapture) + L", release="
				+ std::to_wstring(released) + L", clear="
				+ std::to_wstring(clearAfterRelease) + L", trace="
				+ std::to_wstring(_routedInputTrace.size()) + L", parent="
				+ std::to_wstring(routedSource->GetPresentationWindow() == this) + L", visual="
				+ std::to_wstring(routedSource->IsVisible) + L", hwnd="
				+ std::to_wstring(Handle != nullptr) + L"。");

		_routedInputTrace.clear();
		const bool routeFocusAccepted = routedSource->Focus();
		if (!routeFocusAccepted
			|| GetKeyboardFocusedElement() != routedSource
			|| _routedInputTrace != std::vector<std::wstring>{
				L"keyboard.T+", L"focus+", L"keyboard.B+" }
			|| _lastKeyboardFocusNew != routedSource
			|| !focusOuter->IsFocusScope || !focusScope->IsFocusScope
			|| focusScope->TabNavigation != KeyboardNavigationMode::Cycle
			|| focusScope->DirectionalNavigation
				!= KeyboardNavigationMode::Contained
			|| GetFocusScope(routedSource) != focusScope
			|| GetLogicalFocusedElement(focusScope) != routedSource
			|| GetLogicalFocusedElement(focusOuter) != focusScope
			|| !routedSource->Focusable || !routedSource->IsFocused
			|| !routedSource->IsKeyboardFocused
			|| !routedSource->IsKeyboardFocusWithin
			|| !focusScope->IsFocused || !focusScope->IsKeyboardFocusWithin
			|| !focusOuter->IsFocused || !focusOuter->IsKeyboardFocusWithin
			|| !IsKeyboardFocusWithin
			|| !focusPeerB->Focusable || focusPeerB->IsTabStop
			|| noFocusPeer->Focusable
			|| !routedSource->GetAccessibilitySnapshot().Focused
			|| noFocusPeer->GetAccessibilitySnapshot().Focusable
			|| routedButton->BorderThickness.MaxEdge() < 2.9f
			|| focusOuterBorder->BorderThickness.MaxEdge() < 1.9f)
		{
			std::wstring trace;
			for (const auto& item : _routedInputTrace)
			{
				if (!trace.empty()) trace += L"|";
				trace += item;
			}
			return fail(L"键盘焦点统一切换校验失败：accepted="
				+ std::to_wstring(routeFocusAccepted)
				+ L", owner=" + std::to_wstring(
					GetKeyboardFocusedElement() == routedSource)
				+ L", trace=" + trace
				+ L", logical=" + std::to_wstring(
					routedSource->IsFocused) + L"/"
				+ std::to_wstring(focusScope->IsFocused) + L"/"
				+ std::to_wstring(focusOuter->IsFocused)
				+ L", keyboard=" + std::to_wstring(
					routedSource->IsKeyboardFocused)
				+ L", within=" + std::to_wstring(
					routedSource->IsKeyboardFocusWithin) + L"/"
				+ std::to_wstring(focusScope->IsKeyboardFocusWithin) + L"/"
				+ std::to_wstring(focusOuter->IsKeyboardFocusWithin) + L"/"
				+ std::to_wstring(IsKeyboardFocusWithin)
				+ L", focusable=" + std::to_wstring(routedSource->Focusable)
				+ L"/" + std::to_wstring(focusPeerB->Focusable)
				+ L"/" + std::to_wstring(noFocusPeer->Focusable)
				+ L", border=" + std::to_wstring(
					routedButton->BorderThickness.MaxEdge())
				+ L"/" + std::to_wstring(
					focusOuterBorder->BorderThickness.MaxEdge())
				+ L"。");
		}

		auto* textSource = dynamic_cast<TextBox*>(
			FindGeneratedControlByName(L"wpfTextInputSource"));
		if (!textSource)
			return fail(L"XAML TextInput 演示源不存在。");
		if (!MoveFocus(FocusNavigationDirection::Right)
			|| GetKeyboardFocusedElement() != focusPeerB
			|| !focusPeerB->IsKeyboardFocused || !focusPeerB->IsFocused
			|| routedSource->IsKeyboardFocused || routedSource->IsFocused
			|| !MoveFocus(FocusNavigationDirection::Right)
			|| GetKeyboardFocusedElement() != focusPeerC
			|| MoveFocus(FocusNavigationDirection::Right)
			|| GetKeyboardFocusedElement() != focusPeerC)
			return fail(L"DirectionalNavigation=Contained 未按几何邻接并阻止越界。");
		SetKeyboardFocus(noFocusPeer, false);
		if (GetKeyboardFocusedElement() != focusPeerC
			|| noFocusPeer->IsKeyboardFocused)
			return fail(L"Focusable=false 仍允许 FocusManager 提交键盘焦点。");
		const auto tabOrder = GetTabOrder();
		if (std::find(tabOrder.begin(), tabOrder.end(), focusPeerB)
			!= tabOrder.end()
			|| std::find(tabOrder.begin(), tabOrder.end(), noFocusPeer)
				!= tabOrder.end())
			return fail(L"IsTabStop/Focusable 未从 Tab 候选资格中正确分离。");
		if (!MoveFocus(FocusNavigationDirection::Next)
			|| GetKeyboardFocusedElement() != routedSource)
			return fail(L"TabNavigation=Cycle 未在嵌套 focus scope 内首尾循环。");
		SetKeyboardFocus(focusPeerB, false);
		_routedInputTrace.clear();
		_cancelNextKeyboardFocus = true;
		if (routedSource->Focus()
			|| GetKeyboardFocusedElement() != focusPeerB
			|| GetLogicalFocusedElement(focusScope) != focusPeerB
			|| _lastKeyboardFocusOld != focusPeerB
			|| _lastKeyboardFocusNew != routedSource
			|| _routedInputTrace != std::vector<std::wstring>{
				L"keyboard.T+(cancel)" })
			return fail(L"PreviewGotKeyboardFocus.Handled 未取消键盘焦点事务。");
		SetKeyboardFocus(textSource, false);
		if (GetLogicalFocusedElement(focusOuter) != textSource
			|| GetLogicalFocusedElement(focusScope) != focusPeerB
			|| !focusPeerB->IsFocused || focusPeerB->IsKeyboardFocused
			|| focusScope->IsFocused || focusScope->IsKeyboardFocusWithin
			|| !focusOuter->IsFocused || !focusOuter->IsKeyboardFocusWithin)
			return fail(L"nested focus scope 未分别保留 logical focus。");
		SetKeyboardFocus(focusPeerB, false);
		(void)cui::framework::InputAccess::DispatchInput(
			*this, LifecycleInput(InputReportKind::FocusLost));
		if (GetKeyboardFocusedElement() != nullptr
			|| GetLogicalFocusedElement(focusScope) != focusPeerB
			|| !focusPeerB->IsFocused || focusPeerB->IsKeyboardFocused
			|| focusPeerB->IsKeyboardFocusWithin || IsKeyboardFocusWithin)
			return fail(L"Window 失活未分离 keyboard focus 与 logical focus。");
		(void)cui::framework::InputAccess::DispatchInput(
			*this, LifecycleInput(InputReportKind::FocusGained));
		if (GetKeyboardFocusedElement() != focusPeerB)
			return fail(L"Window 激活未恢复 focus scope 的 logical focus。");
		SetKeyboardFocus(textSource, false);
		(void)cui::framework::WindowAccess::OpenTransientPresentation(
			*this, focusScope,
			TransientPresentationOptions{ false, false, false }, nullptr);
		SetKeyboardFocus(focusPeerC, false);
		(void)cui::framework::WindowAccess::CloseTransientPresentation(
			*this, focusScope);
		if (GetKeyboardFocusedElement() != textSource)
			return fail(L"弹出 focus scope 关闭后未恢复打开前焦点。");
		const auto focusStats = cui::framework::WindowAccess::FocusStatistics(*this);
		const auto inputStats = cui::framework::WindowAccess::InputStatistics(*this);
		if (focusStats.LogicalFocusUpdates < 5
			|| focusStats.NavigationSucceeded < 3
			|| focusStats.ActivationRestores < 1
			|| focusStats.PopupRestores < 1
			|| focusStats.KeyboardTransitionsCanceled < 1
			|| inputStats.KeyboardFocusCanceled < 1)
			return fail(L"FocusManager 统计未覆盖 logical/navigation/restore 路径。");
		_routedInputTrace.clear();
		const auto textBefore = textSource->Text;
		textSource->Select(static_cast<int>(textBefore.size()), 0);
		if (!cui::framework::WindowAccess::TextComposition(*this).CommitText(
			L"A", textSource, TextCompositionInputKind::Programmatic))
			return fail(L"TextCompositionManager 未将确定性文本提交到输入客户端。");
		const std::vector<std::wstring> expectedTextRoute{
			L"text.T outer[A]", L"text.T source",
			L"text.B source", L"text.B outer" };
		const auto textSnapshot = cui::framework::WindowAccess::TextCompositionState(*this);
		if (_routedInputTrace != expectedTextRoute
			|| textSource->Text != textBefore + L"A"
			|| textSnapshot.Stage != TextCompositionStage::Completed
			|| textSnapshot.Text != L"A"
			|| textSnapshot.InputKind
				!= TextCompositionInputKind::Programmatic)
			return fail(L"PreviewTextInput/TextInput 未共享组合文本与路由状态。");

		std::wstring hierarchySummary;
		if (!RunElementHierarchyProbe(&hierarchySummary))
			return fail(L"WPF 元素职责层级探针失败：" + hierarchySummary);

		if (outError) outError->clear();
		return true;
	}
	catch (const std::exception& error)
	{
		return fail(L"声明式特性验证异常："
			+ Convert::StringToWString(error.what()));
	}
}

void DemoWindow::RegisterClassCommandBindings()
{
	CommandBinding exact;
	exact.Command = RoutedCommand(L"Demo.Component.ClassProbe");
	exact.CanExecute = [this](Control* sender, CanExecuteRoutedEventArgs& args)
	{
		HandleClassCommandCanExecute(sender, args);
	};
	exact.Executed = [this](Control* sender, ExecutedRoutedEventArgs& args)
	{
		HandleClassCommandExecuted(sender, args);
	};
	auto exactConnection =
		RoutedCommandManager::RegisterClassCommandBinding(
			DemoWindowGeneratedFeatureCard::ComponentTypeId(),
			std::move(exact));
	if (!exactConnection.Connected())
		ThrowRuntimeError(L"无法注册 FeatureCard 精确 QName class command binding。");
	_classCommandBindingConnections.push_back(std::move(exactConnection));

	CommandBinding nativeFallback;
	nativeFallback.Command = RoutedCommand(L"Demo.Component.ClassProbe");
	nativeFallback.CanExecute = [this](
		Control* sender, CanExecuteRoutedEventArgs& args)
	{
		HandleNativeClassCommandCanExecute(sender, args);
	};
	nativeFallback.Executed = [this](
		Control* sender, ExecutedRoutedEventArgs& args)
	{
		HandleNativeClassCommandExecuted(sender, args);
	};
	auto nativeConnection =
		RoutedCommandManager::RegisterClassCommandBinding(
			UIClass::UI_FrameworkElement, std::move(nativeFallback));
	if (!nativeConnection.Connected())
		ThrowRuntimeError(
			L"无法注册 FrameworkElement native class command fallback。");
	_classCommandBindingConnections.push_back(std::move(nativeConnection));
}

bool DemoWindow::VerifyTextCompositionFeatures(std::wstring* outError)
{
	auto fail = [&](std::wstring message)
	{
		if (outError) *outError = std::move(message);
		return false;
	};
	try
	{
		auto* surface = FindGeneratedControlByName(
			L"textCompositionLabSurface");
		auto* textBox = dynamic_cast<TextBox*>(
			FindGeneratedControlByName(L"compositionTextBox"));
		auto* richTextBox = dynamic_cast<RichTextBox*>(
			FindGeneratedControlByName(L"compositionRichTextBox"));
		auto* passwordBox = dynamic_cast<PasswordBox*>(
			FindGeneratedControlByName(L"compositionPasswordBox"));
		if (!surface || !textBox || !richTextBox || !passwordBox
			|| !FindGeneratedControlByName(L"compositionState")
			|| !FindGeneratedControlByName(L"compositionStats")
			|| !FindGeneratedControlByName(L"compositionTrace"))
			return fail(L"TextComposition/IME XAML 实验台未完整生成。");
		TabItem* compositionPage = nullptr;
		for (auto* current = surface; current;
			current = current->GetRoutedParent())
		{
			if (auto* page = dynamic_cast<TabItem*>(current))
			{
				compositionPage = page;
				break;
			}
		}
		const int compositionPageIndex = _tabs && compositionPage
			? _tabs->IndexOfItem(compositionPage) : -1;
		if (!_tabs || compositionPageIndex < 0)
			return fail(L"TextComposition/IME 实验台未连接到 TabControl。");
		const int originalTabIndex = _tabs->SelectedIndex;
		struct TextCompositionTabScope final
		{
			TabControl& Tabs;
			int SelectedIndex;

			~TextCompositionTabScope()
			{
				Tabs.SelectedIndex = SelectedIndex;
			}
		} restoreTab{ *_tabs, originalTabIndex };
		_tabs->SelectedIndex = compositionPageIndex;
		RequestLayout();
		UpdateLayout();

		auto& manager = cui::framework::WindowAccess::TextComposition(*this);
		const auto* previousFocus = GetKeyboardFocusedElement();
		const auto originalText = textBox->Text;
		const auto originalRichText = richTextBox->Text;
		const auto originalPassword = passwordBox->Password;
		const int originalTextSelectionStart = textBox->GetSelectionStart();
		const int originalTextSelectionLength = textBox->SelectionLength;
		const int originalRichSelectionStart = richTextBox->GetSelectionStart();
		const int originalRichSelectionLength = richTextBox->SelectionLength;
		const int originalPasswordSelectionStart = passwordBox->GetSelectionStart();
		const int originalPasswordSelectionLength = passwordBox->SelectionLength;

		manager.Reset();
		_compositionPreviewHandled = false;
		_textCompositionTrace.clear();
		textBox->Text = L"";
		textBox->Select(0, 0);
		richTextBox->Text = L"";
		richTextBox->Select(0, 0);
		passwordBox->Password = L"";
		passwordBox->Select(0, 0);
		SetKeyboardFocus(textBox, false);

		const auto statisticsBefore = cui::framework::WindowAccess::TextCompositionStatisticsOf(*this);
		if (!manager.StartComposition(
			textBox, TextCompositionInputKind::Programmatic))
			return fail(L"TextComposition Start 事务未建立。");
		const auto started = cui::framework::WindowAccess::TextCompositionState(*this);
		if (!started.IsComposing || started.Source != textBox
			|| started.Stage != TextCompositionStage::Started
			|| started.CompositionId == 0 || started.CaretIndex != 0)
			return fail(L"TextComposition Start 快照不完整。");
		const std::vector<unsigned char> attributes{ 0, 1 };
		const std::vector<std::uint32_t> clauses{ 0, 2 };
		if (!manager.UpdateComposition(
			L"ni", 1, attributes, clauses, L"system", L"control"))
			return fail(L"TextComposition Update 事务未发布。");
		const auto updated = cui::framework::WindowAccess::TextCompositionState(*this);
		if (!updated.IsComposing || updated.Source != textBox
			|| updated.CompositionId != started.CompositionId
			|| updated.Stage != TextCompositionStage::Updated
			|| updated.CompositionText != L"ni" || updated.CaretIndex != 1
			|| updated.Attributes != attributes || updated.Clauses != clauses
			|| updated.SystemText != L"system"
			|| updated.ControlText != L"control" || !textBox->Text.empty())
			return fail(L"TextComposition Update 未保持预编辑、caret 或 IMM 元数据。");

		const std::wstring committedText = L"\u4F60\U0001F600";
		if (!manager.CompleteComposition(
			committedText, L"system-result", L"control-result"))
			return fail(L"TextComposition Complete 未交给 TextBox text client。");
		const auto completed = cui::framework::WindowAccess::TextCompositionState(*this);
		const auto idText = std::to_wstring(started.CompositionId);
		const std::vector<std::wstring> expectedLifecycle{
			L"T.Start #" + idText,
			L"B.Start #" + idText,
			L"T.Update #" + idText + L" [ni] @1",
			L"B.Update #" + idText + L" [ni] @1",
			L"T.Commit #" + idText + L" [" + committedText + L"]",
			L"B.Commit #" + idText + L" applied" };
		if (textBox->Text != committedText
			|| completed.IsComposing || completed.Source != nullptr
			|| completed.CompositionId != started.CompositionId
			|| completed.Stage != TextCompositionStage::Completed
			|| completed.Text != committedText
			|| completed.SystemText != L"system-result"
			|| completed.ControlText != L"control-result"
			|| _textCompositionTrace != expectedLifecycle)
			return fail(L"六阶段 tunnel/behavior/bubble 顺序或完整 UTF-16 提交不正确。");
		const auto normalStatistics = cui::framework::WindowAccess::TextCompositionStatisticsOf(*this);
		if (normalStatistics.CompositionsStarted
			!= statisticsBefore.CompositionsStarted + 1
			|| normalStatistics.CompositionsUpdated
				!= statisticsBefore.CompositionsUpdated + 1
			|| normalStatistics.CompositionsCompleted
				!= statisticsBefore.CompositionsCompleted + 1
			|| normalStatistics.TextCommits
				!= statisticsBefore.TextCommits + 1
			|| normalStatistics.TextApplications
				!= statisticsBefore.TextApplications + 1)
			return fail(L"TextComposition 生命周期统计未形成单次事务闭环。");

		_textCompositionTrace.clear();
		_compositionPreviewHandled = true;
		const auto beforeBlocked = textBox->Text;
		if (manager.CommitText(
			L"blocked", textBox, TextCompositionInputKind::Programmatic)
			|| textBox->Text != beforeBlocked
			|| _textCompositionTrace.size() != 1
			|| _textCompositionTrace.front().find(L"handled")
				== std::wstring::npos
			|| cui::framework::WindowAccess::TextCompositionStatisticsOf(*this).PreviewApplicationsSuppressed
				!= normalStatistics.PreviewApplicationsSuppressed + 1)
			return fail(L"PreviewTextInput.Handled 未阻止默认编辑或仍发生 bubble。");
		_compositionPreviewHandled = false;

		_textCompositionTrace.clear();
		(void)cui::framework::InputAccess::DispatchInput(
			*this, LifecycleInput(InputReportKind::FocusGained));
		SetKeyboardFocus(textBox, false);
		if (GetKeyboardFocusedElement() != textBox)
			return fail(L"WM_CHAR 代理对探针未能建立文本输入焦点。");
		const auto beforeSurrogate = textBox->Text;
		const auto high = manager.ProcessWindowMessage(
			WM_CHAR, static_cast<WPARAM>(0xD83D), 0);
		if (!high.Recognized || high.CallDefaultWindowProcedure
			|| high.TextApplied || textBox->Text != beforeSurrogate)
			return fail(L"WM_CHAR 高代理项被过早提交。");
		const auto low = manager.ProcessWindowMessage(
			WM_CHAR, static_cast<WPARAM>(0xDE00), 0);
		if (!low.Recognized || low.CallDefaultWindowProcedure
			|| !low.TextApplied
			|| textBox->Text != beforeSurrogate + L"\U0001F600")
			return fail(L"WM_CHAR 代理对未作为一个完整文本提交：recognized="
				+ std::to_wstring(low.Recognized)
				+ L", default=" + std::to_wstring(
					low.CallDefaultWindowProcedure)
				+ L", applied=" + std::to_wstring(low.TextApplied)
				+ L", focus=" + std::to_wstring(
					GetKeyboardFocusedElement() == textBox)
				+ L", before=[" + beforeSurrogate
				+ L"], actual=[" + textBox->Text
				+ L"], expected=[" + beforeSurrogate + L"\U0001F600"
				+ L"]。");

		(void)cui::framework::InputAccess::DispatchInput(
			*this, LifecycleInput(InputReportKind::FocusGained));
		SetKeyboardFocus(textBox, false);
		if (GetKeyboardFocusedElement() != textBox)
			return fail(L"WM_UNICHAR 探针未能建立文本输入焦点。");
		const auto beforeUnicode = textBox->Text;
		const auto capability = manager.ProcessWindowMessage(
			WM_UNICHAR, UNICODE_NOCHAR, 0);
		const auto unicode = manager.ProcessWindowMessage(
			WM_UNICHAR, static_cast<WPARAM>(0x1F642), 0);
		if (!capability.Recognized || capability.CallDefaultWindowProcedure
			|| capability.Result != TRUE || capability.TextApplied
			|| !unicode.Recognized || unicode.CallDefaultWindowProcedure
			|| !unicode.TextApplied
			|| textBox->Text != beforeUnicode + L"\U0001F642")
			return fail(L"WM_UNICHAR 能力探测或 Unicode scalar 提交不正确。");

		_textCompositionTrace.clear();
		if (!manager.StartComposition(
			textBox, TextCompositionInputKind::Programmatic)
			|| !manager.UpdateComposition(L"cancel-me", 4))
			return fail(L"显式取消探针无法建立组合事务。");
		const auto cancelTraceCount = _textCompositionTrace.size();
		manager.CancelComposition(TextCompositionCancelReason::Explicit);
		const auto canceled = cui::framework::WindowAccess::TextCompositionState(*this);
		if (canceled.IsComposing
			|| canceled.Stage != TextCompositionStage::Canceled
			|| canceled.CancelReason != TextCompositionCancelReason::Explicit
			|| _textCompositionTrace.size() != cancelTraceCount)
			return fail(L"Cancel 应为 manager 生命周期，不应伪造公开 routed event。");

		SetKeyboardFocus(textBox, false);
		if (!manager.StartComposition(
			textBox, TextCompositionInputKind::Programmatic)
			|| !manager.UpdateComposition(L"focus", 5))
			return fail(L"焦点取消探针无法建立组合事务。");
		SetKeyboardFocus(richTextBox, false, FocusChangeReason::Programmatic);
		const auto focusCanceled = cui::framework::WindowAccess::TextCompositionState(*this);
		if (focusCanceled.IsComposing
			|| focusCanceled.Stage != TextCompositionStage::Canceled
			|| focusCanceled.CancelReason
				!= TextCompositionCancelReason::FocusChanged
			|| GetKeyboardFocusedElement() != richTextBox)
			return fail(L"输入源焦点切换未先完成/取消 TextComposition 事务。");

		// RichTextBox must format and scroll its new caret during the input
		// transaction, when no presentation DrawingContext is active.
		_textCompositionTrace.clear();
		const std::wstring richCommit = L"Rich \u6587\U0001F642";
		if (!manager.CommitText(
			richCommit, richTextBox,
			TextCompositionInputKind::Programmatic)
			|| richTextBox->Text != richCommit
			|| richTextBox->GetCaretIndex()
				!= static_cast<int>(richCommit.size()))
			return fail(L"RichTextBox 帧外文本格式化、插入或 caret 滚动失败。");

		_textCompositionTrace.clear();
		SetKeyboardFocus(passwordBox, false);
		const std::wstring secret = L"\u5BC6\U0001F512";
		if (!manager.CommitText(
			secret, passwordBox, TextCompositionInputKind::Programmatic)
			|| passwordBox->Password != secret)
			return fail(L"PasswordBox 未接入统一 TextComposition client。");
		for (const auto& token : _textCompositionTrace)
		{
			if (token.find(secret) != std::wstring::npos)
				return fail(L"PasswordBox routed trace 泄漏了输入正文。");
		}
		if (_textCompositionTrace.size() != 2
			|| _textCompositionTrace.front().find(L"<secure:")
				== std::wstring::npos)
			return fail(L"PasswordBox trace 未使用安全长度摘要。");

		manager.Reset();
		_compositionPreviewHandled = false;
		textBox->Text = originalText;
		textBox->Select(
			originalTextSelectionStart, originalTextSelectionLength);
		richTextBox->Text = originalRichText;
		richTextBox->Select(
			originalRichSelectionStart, originalRichSelectionLength);
		passwordBox->Password = originalPassword;
		passwordBox->Select(
			originalPasswordSelectionStart, originalPasswordSelectionLength);
		SetKeyboardFocus(const_cast<Control*>(previousFocus), false);
		RefreshTextCompositionSummary();
		if (outError) outError->clear();
		return true;
	}
	catch (const std::exception& error)
	{
		return fail(L"TextComposition 特性验证异常："
			+ Convert::StringToWString(error.what()));
	}
}

bool DemoWindow::VerifyPresentationFeatures(std::wstring* outError)
{
	auto fail = [&](std::wstring message)
	{
		if (outError) *outError = std::move(message);
		return false;
	};
	try
	{
		auto* surface = dynamic_cast<NativeSurface*>(
			FindGeneratedControlByName(
				L"presentationProbeSurface"));
		auto* behavior = surface ? dynamic_cast<PresentationProbeBehavior*>(
			surface->Behavior()) : nullptr;
		auto* topologyTile = FindGeneratedControlByName(
			L"presentationTopologyTile");
		auto* loadingRing = dynamic_cast<LoadingRing*>(
			FindGeneratedControlByName(L"loadingRing"));
		auto* richTextControl = FindGeneratedControlByName(L"splitNotes");
		auto* richText = dynamic_cast<RichTextBox*>(richTextControl);
		auto* scroll = dynamic_cast<ScrollViewer*>(
			FindGeneratedControlByName(L"demoScroll"));
		auto* scrollProbe =
			FindGeneratedControlByName(L"farButton");
		if (!_tabs || !surface || !behavior || !behavior->Attached()
			|| !topologyTile || !loadingRing || !richText
			|| !scroll || !scrollProbe
			|| !Handle)
		{
			std::wstring missing;
			auto appendMissing = [&](bool present, std::wstring_view name)
			{
				if (!present)
				{
					if (!missing.empty()) missing += L", ";
					missing += name;
				}
			};
			appendMissing(_tabs != nullptr, L"mainTabs");
			appendMissing(surface != nullptr, L"presentationProbeSurface");
			appendMissing(behavior != nullptr, L"PresentationProbeBehavior");
			appendMissing(behavior && behavior->Attached(), L"behavior.Attach");
			appendMissing(topologyTile != nullptr, L"presentationTopologyTile");
			appendMissing(loadingRing != nullptr, L"loadingRing");
			appendMissing(richTextControl != nullptr, L"splitNotes field");
			appendMissing(richText != nullptr, L"splitNotes RichTextBox RTTI");
			appendMissing(scroll != nullptr, L"demoScroll");
			appendMissing(scrollProbe != nullptr, L"farButton");
			appendMissing(Handle != nullptr, L"HWND");
			return fail(L"Presentation smoke 缺少真实 Window 或 XAML behavior："
				+ missing + L"。");
		}
		// The render smoke normally owns a hidden HWND. Only the input/frame gates
		// below temporarily expose it: keeping the whole method visible lets an
		// active LoadingRing continuously enqueue another frame while a drain is
		// trying to reach quiescence.
		struct VisibleWindowScope final
		{
			HWND Handle = nullptr;
			bool WasVisible = false;

			explicit VisibleWindowScope(HWND handle)
				: Handle(handle),
				  WasVisible(handle && ::IsWindowVisible(handle) != FALSE)
			{
				if (!WasVisible && Handle && ::IsWindow(Handle))
					(void)::ShowWindow(Handle, SW_SHOWNOACTIVATE);
			}

			void Restore() noexcept
			{
				if (!Handle) return;
				if (!WasVisible && ::IsWindow(Handle))
					(void)::ShowWindow(Handle, SW_HIDE);
				Handle = nullptr;
			}

			~VisibleWindowScope()
			{
				Restore();
			}
		};
		auto& richDocument = richText->GetDocument();
		auto* centeredParagraph = richDocument.GetBlocks().Count() > 0
			? dynamic_cast<Paragraph*>(richDocument.GetBlocks().At(0)) : nullptr;
		auto* justifiedParagraph = richDocument.GetBlocks().Count() > 1
			? dynamic_cast<Paragraph*>(richDocument.GetBlocks().At(1)) : nullptr;
		auto* rightParagraph = richDocument.GetBlocks().Count() > 3
			? dynamic_cast<Paragraph*>(richDocument.GetBlocks().At(3)) : nullptr;
		auto* rtlParagraph = richDocument.GetBlocks().Count() > 4
			? dynamic_cast<Paragraph*>(richDocument.GetBlocks().At(4)) : nullptr;
		if (richDocument.GetBlocks().Count() != 5
			|| !centeredParagraph || !justifiedParagraph || !rightParagraph
			|| !rtlParagraph
			|| centeredParagraph->GetTextAlignment() != TextAlignment::Center
			|| justifiedParagraph->GetTextAlignment() != TextAlignment::Justify
			|| rightParagraph->GetTextAlignment() != TextAlignment::Right
			|| rtlParagraph->GetFlowDirection()
				!= FlowDirection::RightToLeft)
		{
			return fail(L"RichTextBox XAML 段落 TextAlignment/FlowDirection 未完整物化。");
		}
		const auto richFlat = richDocument.Flatten();
		const auto stretchedText = richFlat.Text.find(L"扩展字宽");
		const auto japaneseText = richFlat.Text.find(L"日本語区域");
		if (stretchedText == std::wstring::npos
			|| RichTextDocument(richFlat).StyleAt(stretchedText).FontStretch
				!= std::optional<DWRITE_FONT_STRETCH>(
					DWRITE_FONT_STRETCH_EXPANDED)
			|| japaneseText == std::wstring::npos
			|| RichTextDocument(richFlat).StyleAt(japaneseText).Language
				!= std::optional<std::wstring>(L"ja-jp"))
		{
			return fail(L"RichTextBox XAML FontStretch/Language 未进入实际文档格式。");
		}
		auto findOwningTabIndex = [this](Control* descendant)
		{
			for (auto* current = descendant; current;
				current = current->GetVisualParent())
				for (size_t index = 0; index < _tabs->ItemCount(); ++index)
					if (_tabs->GetItem(static_cast<int>(index)) == current)
						return static_cast<int>(index);
			return -1;
		};
		const int containerTabIndex = findOwningTabIndex(loadingRing);
		const int layoutTabIndex = findOwningTabIndex(scroll);
		const int browserTabIndex = findOwningTabIndex(_web);
		const int presentationTabIndex = findOwningTabIndex(surface);
		if (containerTabIndex < 0 || layoutTabIndex < 0
			|| browserTabIndex < 0 || presentationTabIndex < 0)
			return fail(L"Presentation smoke 无法解析 XAML TabItem 所有权。");
		const int originalIndex = _tabs->SelectedIndex;
		struct TabStateScope final
		{
			TabControl& Tabs;
			int SelectedIndex;
			ScrollViewer& Scroll;
			double HorizontalOffset;
			double VerticalOffset;

			~TabStateScope()
			{
				Scroll.ScrollToHorizontalOffset(HorizontalOffset);
				Scroll.ScrollToVerticalOffset(VerticalOffset);
				Tabs.SelectedIndex = SelectedIndex;
			}
		} restoreTabs{
			*_tabs, originalIndex, *scroll,
			scroll->HorizontalOffset, scroll->VerticalOffset };

		RECT client{};
		::GetClientRect(Handle, &client);
		auto drainPresentationWork = [&]()
		{
			for (int pass = 0; pass < 8; ++pass)
			{
				RECT pending{};
				const bool osPending =
					::GetUpdateRect(Handle, &pending, FALSE) != FALSE;
				if (!osPending
					&& !cui::framework::WindowAccess::
						HasPendingRenderWork(*this))
					return true;
				(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
			}
			RECT pending{};
			return ::GetUpdateRect(Handle, &pending, FALSE) == FALSE
				&& !cui::framework::WindowAccess::
					HasPendingRenderWork(*this);
		};
		// Window keeps this dispatch private because it is an implementation detail.
		// The real-HWND smoke deliberately pumps only that coalesced presentation
		// turn, never arbitrary queued input, so a drag remains active across paint.
		const UINT presentationDispatchMessage =
			cui::framework::WindowAccess::
				PresentationDispatchMessageForTesting();
		auto dispatchPresentationTurn = [&]()
		{
			MSG message{};
			if (::PeekMessageW(
				&message, Handle,
				presentationDispatchMessage, presentationDispatchMessage,
				PM_REMOVE) == FALSE)
				return false;
			(void)::DispatchMessageW(&message);
			return true;
		};
		auto clearPresentationTurns = [&]()
		{
			for (int pass = 0; pass < 8 && dispatchPresentationTurn(); ++pass)
			{
			}
		};

		// DataGrid must participate in the real retained layout/input tree. This
		// gate intentionally uses HWND messages rather than direct API dispatch.
		auto* dataGrid = dynamic_cast<DataGrid*>(
			FindGeneratedControlByName(L"demoDataGrid"));
		if (!dataGrid
			|| !_tabs->SelectItem(PageIndex(DemoPage::DataGrid)))
			return fail(L"Presentation smoke 无法切换到 DataGrid 示例页。");
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid 示例页未完成稳定 retained 绘制。");
		(void)dataGrid->ApplyTemplate();
		// The synthetic new-item placeholder is not row data.  Exercise its
		// authored AOT cell templates directly so row-independent templates cannot
		// leak misleading values into the otherwise empty sentinel.  Keep the Cell
		// containers alive: their routed input is what starts AddNew on the second
		// click.
		const size_t placeholderCount = dataGrid->ItemCount();
		if (placeholderCount == 0)
			return fail(L"DataGrid 新增占位行验证缺少项。");
		const size_t placeholderIndex = placeholderCount - 1;
		if (!dataGrid->SetCurrentCell(placeholderIndex, 1)
			|| !dataGrid->ScrollIntoView(dataGrid->GetCurrentItem()))
			return fail(L"DataGrid 新增占位行无法定位到首个可编辑单元格。");
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid 新增占位行未完成 retained 绘制。");
		auto* placeholderRow = dynamic_cast<DataGridRow*>(
			dataGrid->GetGeneratedItem(placeholderIndex));
		auto* placeholderCustomerCell = placeholderRow
			? placeholderRow->GetCell(1) : nullptr;
		auto* placeholderPaidCell = placeholderRow
			? placeholderRow->GetCell(6) : nullptr;
		if (placeholderCustomerCell)
			(void)placeholderCustomerCell->ApplyTemplate();
		if (placeholderPaidCell) (void)placeholderPaidCell->ApplyTemplate();
		auto* placeholderPrompt = placeholderCustomerCell
			? dynamic_cast<Label*>(
				placeholderCustomerCell->FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"PART_NewItemPrompt"))) : nullptr;
		auto* placeholderCustomerPresenter = placeholderCustomerCell
			? placeholderCustomerCell->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_ContentPresenter")) : nullptr;
		auto* placeholderPaidPresenter = placeholderPaidCell
			? placeholderPaidCell->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_ContentPresenter")) : nullptr;
		if (!placeholderRow || !placeholderRow->GetIsNewItem()
			|| !placeholderCustomerCell || !placeholderPaidCell
			|| !placeholderCustomerCell->GetIsNewItemPlaceholder()
			|| !placeholderPaidCell->GetIsNewItemPlaceholder()
			|| !placeholderPrompt
			|| placeholderPrompt->Text != L"＋ 双击新增"
			|| placeholderPrompt->Visibility != Visibility::Visible
			|| !placeholderCustomerPresenter
			|| placeholderCustomerPresenter->Visibility != Visibility::Collapsed
			|| !placeholderPaidPresenter
			|| placeholderPaidPresenter->Visibility != Visibility::Collapsed)
			return fail(L"DataGrid 新增占位行未隐藏普通列内容或未显示新增提示：row="
				+ std::to_wstring(placeholderRow != nullptr) + L"/"
				+ std::to_wstring(
					placeholderRow && placeholderRow->GetIsNewItem())
				+ L"，cell=" + std::to_wstring(
					placeholderCustomerCell != nullptr) + L"/"
				+ std::to_wstring(placeholderCustomerCell
					&& placeholderCustomerCell->GetIsNewItemPlaceholder())
				+ L"，prompt=" + std::to_wstring(
					placeholderPrompt != nullptr) + L"/"
				+ std::to_wstring(placeholderPrompt
					&& placeholderPrompt->Visibility == Visibility::Visible)
				+ L"，presenter=" + std::to_wstring(
					placeholderCustomerPresenter != nullptr) + L"/"
				+ std::to_wstring(placeholderCustomerPresenter
					&& placeholderCustomerPresenter->Visibility
						== Visibility::Collapsed) + L"。");
		// Restore the first row before taking identities used by the remaining
		// input and presentation probes; virtualization may recycle both rows.
		if (!dataGrid->SetCurrentCell(0, 3)
			|| !dataGrid->ScrollIntoView(dataGrid->GetCurrentItem()))
			return fail(L"DataGrid 新增占位行验证后无法恢复首行。");
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid 新增占位行验证后首行未稳定。");
		auto* headerPresenter = dynamic_cast<DataGridColumnHeadersPresenter*>(
			dataGrid->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_ColumnHeadersPresenter")));
		auto* firstHeader = headerPresenter
			? dynamic_cast<DataGridColumnHeader*>(
				headerPresenter->GetVisualChild(0)) : nullptr;
		auto* firstRow = dynamic_cast<DataGridRow*>(
			dataGrid->GetGeneratedItem(0));
		auto* customerCell = firstRow ? firstRow->GetCell(1) : nullptr;
		auto* stageCell = firstRow ? firstRow->GetCell(3) : nullptr;
		auto* stageDisplay = stageCell ? dynamic_cast<ComboBox*>(
			stageCell->GetVisualContent()) : nullptr;
		if (stageDisplay) (void)stageDisplay->ApplyTemplate();
		auto* stageSelectionFace = stageDisplay ? dynamic_cast<Label*>(
			stageDisplay->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_SelectionBox"))) : nullptr;
		auto* stageDisplayGlyph = stageDisplay ? dynamic_cast<Label*>(
			stageDisplay->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_DropDownGlyph"))) : nullptr;
		if (!headerPresenter || !firstHeader || !firstRow || !customerCell
			|| firstRow->GetCells().size() != dataGrid->ColumnCount())
			return fail(
				L"DataGrid 模板未生成表头 presenter、真实 Row 或 Cell 视觉树。");
		auto* customerHeader = headerPresenter->GetHeader(1);
		auto* customerHeaderText = customerHeader
			? dynamic_cast<Label*>(
				cui::framework::TemplateAccess::GetGeneratedContent(
					*customerHeader)) : nullptr;
		const auto customerHeaderTextSize = customerHeaderText
			? customerHeaderText->GetActualSizeDip() : cui::core::Size{};
		if (!customerHeader || !customerHeader->LastContentError().empty()
			|| !customerHeaderText
			|| customerHeaderText->Text != L"客户 / Customer"
			|| customerHeaderTextSize.width <= 0.0f
			|| customerHeaderTextSize.height <= 0.0f)
			return fail(L"DataGrid 客户列 HeaderTemplate 未生成可见文本：text="
				+ (customerHeaderText ? customerHeaderText->Text : std::wstring{})
				+ L"，size=" + std::to_wstring(customerHeaderTextSize.width)
				+ L"x" + std::to_wstring(customerHeaderTextSize.height)
				+ L"，error=" + (customerHeader
					? customerHeader->LastContentError() : std::wstring{}));
		if (!stageCell || !stageDisplay || !stageSelectionFace
			|| !stageDisplayGlyph || stageDisplayGlyph->Text != L"\xE70D")
			return fail(L"DataGrid ComboBox retained 验证缺少显示视觉：cell="
				+ std::to_wstring(stageCell != nullptr)
				+ L"，combo=" + std::to_wstring(stageDisplay != nullptr)
				+ L"，face=" + std::to_wstring(stageSelectionFace != nullptr)
				+ L"，style=" + (stageDisplay
					? cui::framework::StyleAccess::ResourceKey(*stageDisplay)
					: std::wstring{})
				+ L"，automatic=" + std::to_wstring(stageDisplay
					&& cui::framework::StyleAccess::
						ResourceKeyIsAutomatic(*stageDisplay))
				+ L"，visibleRules=" + std::to_wstring(stageDisplay
					&& cui::framework::StyleAccess::
						HasVisibleStyleRules(*stageDisplay))
				+ L"，theme=" + std::to_wstring(stageDisplay
					&& static_cast<bool>(cui::framework::StyleAccess::
						Theme(*stageDisplay)))
				+ L"，document=" + std::to_wstring(stageDisplay
					&& static_cast<bool>(cui::framework::StyleAccess::
						DocumentStyles(*stageDisplay)))
				+ L"，template=" + std::to_wstring(
					stageDisplay && stageDisplay->GetTemplate() ? 1 : 0)
				+ L"，templateError=" + (stageDisplay
					? stageDisplay->LastTemplateError() : std::wstring{}));
		(void)cui::framework::WindowAccess::PresentationOrder(
			*this, stageSelectionFace);
		PresentationNodeSnapshot stageSelectionSnapshot{};
		const bool hasStageSelectionSnapshot =
			cui::framework::WindowAccess::TryGetPresentationNodeSnapshot(
				*this, stageSelectionFace, stageSelectionSnapshot);
		PresentationNodeSnapshot stageGlyphSnapshot{};
		const bool hasStageGlyphSnapshot =
			cui::framework::WindowAccess::TryGetPresentationNodeSnapshot(
				*this, stageDisplayGlyph, stageGlyphSnapshot);
		const auto stageForeground =
			stageSelectionFace->GetComputedForegroundBrush();
		const auto stageGlyphForeground =
			stageDisplayGlyph->GetComputedForegroundBrush();
		const auto stageFaceSize = stageSelectionFace->GetActualSizeDip();
		const auto stageGlyphSize = stageDisplayGlyph->GetActualSizeDip();
		if (stageDisplay->Text.empty()
			|| stageSelectionFace->Text != stageDisplay->Text
			|| !stageSelectionFace->GetIsVisible()
			|| stageFaceSize.width <= 0.0f || stageFaceSize.height <= 0.0f
			|| stageForeground.Kind != cui::drawing::BrushKind::Solid
			|| stageForeground.Color.a * stageForeground.Opacity <= 0.8f
			|| !hasStageSelectionSnapshot
			|| !stageSelectionSnapshot.HasPresented
			|| stageGlyphSize.width <= 0.0f || stageGlyphSize.height <= 0.0f
			|| stageGlyphForeground.Kind != cui::drawing::BrushKind::Solid
			|| stageGlyphForeground.Color.a
				* stageGlyphForeground.Opacity <= 0.8f
			|| !hasStageGlyphSnapshot || !stageGlyphSnapshot.HasPresented)
			return fail(L"DataGrid ComboBox 显示面未录入可见 retained 文本：text="
				+ stageDisplay->Text + L"，face=" + stageSelectionFace->Text
				+ L"，visible=" + std::to_wstring(
					stageSelectionFace->GetIsVisible())
				+ L"，size=" + std::to_wstring(stageFaceSize.width)
				+ L"x" + std::to_wstring(stageFaceSize.height)
				+ L"，foreground=" + std::to_wstring(
					stageForeground.Color.a * stageForeground.Opacity)
				+ L"，snapshot=" + std::to_wstring(
					hasStageSelectionSnapshot)
				+ L"/" + std::to_wstring(stageSelectionSnapshot.HasPresented)
				+ L"/" + std::to_wstring(
					stageSelectionSnapshot.HasDrawingCommands)
				+ L"，native=" + std::to_wstring(
					stageSelectionSnapshot.NativeComposition));
		// Star columns must be resolved once by DataGrid and projected as the
		// same pixel widths into both header and row grids.  Comparing the real
		// themed first frame prevents a fixed-width-only unit test from hiding a
		// bounded-header/unbounded-row split.
		for (size_t columnIndex = 0;
			columnIndex < dataGrid->ColumnCount(); ++columnIndex)
		{
			auto* header = headerPresenter->GetHeader(columnIndex);
			auto* cell = firstRow->GetCell(columnIndex);
			if (!header || !cell)
				return fail(L"DataGrid 首帧列宽验证缺少表头或单元格。");
			const auto headerBounds = header->GetAbsoluteBoundsDip();
			const auto currentCellBounds = cell->GetAbsoluteBoundsDip();
			if (std::abs(headerBounds.left - currentCellBounds.left) > 1.1f
				|| std::abs(headerBounds.right - currentCellBounds.right) > 1.1f)
				return fail(
					L"DataGrid 首帧列与列头未共享显示宽度：column="
					+ std::to_wstring(columnIndex)
					+ L"，header=" + std::to_wstring(headerBounds.left)
					+ L"/" + std::to_wstring(headerBounds.right)
					+ L"，cell=" + std::to_wstring(currentCellBounds.left)
					+ L"/" + std::to_wstring(currentCellBounds.right) + L"。");
		}

		// Exercise the real AOT/native-column path before the general DataGrid
		// input probes.  The display element is generated after the framework
		// theme transaction, while the editor and Popup are generated later still;
		// all three must receive the same compiled theme environment.
		const std::wstring originalStageText = stageDisplay->Text;
		const auto stageCellBounds = stageCell->GetRenderedAbsoluteRectDip();
		MouseEventArgs stageOpenInput(
			MouseButton::Left, MouseButtonState::Pressed, 1,
			static_cast<int>((stageCellBounds.right - stageCellBounds.left) * 0.5f),
			static_cast<int>((stageCellBounds.bottom - stageCellBounds.top) * 0.5f),
			0);
		stageOpenInput.HasRootPosition = true;
		stageOpenInput.RootX =
			(stageCellBounds.left + stageCellBounds.right) * 0.5f;
		stageOpenInput.RootY =
			(stageCellBounds.top + stageCellBounds.bottom) * 0.5f;
		if (!dataGrid->SetCurrentCell(0, 3)
			|| !dataGrid->BeginEdit(&stageOpenInput))
			return fail(L"DataGrid ComboBox 无法进入编辑态。");
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		stageCell = firstRow ? firstRow->GetCell(3) : nullptr;
		auto* stageEditor = stageCell ? dynamic_cast<ComboBox*>(
			stageCell->GetEditingElement()) : nullptr;
		if (!stageEditor || !stageEditor->GetIsDropDownOpen())
			return fail(L"DataGrid ComboBox 鼠标编辑未同步展开主题模板。");
		SetKeyboardFocus(stageEditor, false);
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid ComboBox 编辑模板未完成 Production 布局。");
		auto* editingSelectionFace = dynamic_cast<Label*>(
			stageEditor->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_SelectionBox")));
		auto* editingChrome = dynamic_cast<Border*>(
			stageEditor->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_ComboBoxChrome")));
		auto* editingDropDownToggle = stageEditor->FindDeclarativeTemplatePart(
			MakeTemplatePartToken(L"PART_DropDownToggle"));
		if (!editingSelectionFace || !editingChrome
			|| !editingDropDownToggle
			|| editingDropDownToggle->GetActualSizeDip().width <= 0.0f
			|| stageEditor->Background.Kind
				!= cui::drawing::BrushKind::Solid
			|| stageEditor->Background.Color.a
				* stageEditor->Background.Opacity <= 0.99f
			|| stageEditor->BorderThickness != Thickness(1.0f)
			|| editingSelectionFace->Text != originalStageText
			|| editingSelectionFace->GetActualSizeDip().width <= 0.0f)
			return fail(
				L"DataGrid ComboBox 编辑态未保留可见边界或选择文本。");

		// Establish native activation before opening the transient surface.  A
		// synthetic first pointer-down must not spend its turn activating the HWND
		// and thereby dismiss the Popup before the item receives routed input.
		(void)::SetActiveWindow(Handle);
		(void)::SetFocus(Handle);
		if (::GetActiveWindow() != Handle || ::GetFocus() != Handle)
			return fail(L"DataGrid ComboBox 真实选择输入未建立 HWND 焦点。");
		SetKeyboardFocus(stageEditor, false);
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid ComboBox Popup 未完成 retained 绘制。");
		auto* dropDownBorder = dynamic_cast<Border*>(
			stageEditor->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_DropDownBorder")));
		auto* secondRow = dynamic_cast<DataGridRow*>(
			dataGrid->GetGeneratedItem(1));
		auto* coveredStageCell = secondRow ? secondRow->GetCell(3) : nullptr;
		auto* coveredStageDisplay = coveredStageCell
			? dynamic_cast<ComboBox*>(coveredStageCell->GetVisualContent()) : nullptr;
		if (coveredStageDisplay) (void)coveredStageDisplay->ApplyTemplate();
		if (!dropDownBorder || !coveredStageDisplay
			|| coveredStageDisplay->Text.empty())
			return fail(
				L"DataGrid ComboBox Popup 缺少边框或被覆盖的文本单元格。");
		const auto popupBounds = dropDownBorder->GetRenderedAbsoluteRectDip();
		const auto coveredBounds =
			coveredStageCell->GetRenderedAbsoluteRectDip();
		const bool popupOverlapsTextCell =
			popupBounds.left < coveredBounds.right
			&& popupBounds.right > coveredBounds.left
			&& popupBounds.top < coveredBounds.bottom
			&& popupBounds.bottom > coveredBounds.top;
		const int coveredPresentationOrder =
			cui::framework::WindowAccess::PresentationOrder(
				*this, coveredStageDisplay);
		const int popupPresentationOrder =
			cui::framework::WindowAccess::PresentationOrder(
				*this, dropDownBorder);
		PresentationNodeSnapshot popupSnapshot{};
		const bool hasPopupSnapshot =
			cui::framework::WindowAccess::TryGetPresentationNodeSnapshot(
				*this, dropDownBorder, popupSnapshot);
		bool popupContainsGeneratedItems = true;
		for (size_t index = 0; index < stageEditor->ItemCount(); ++index)
		{
			auto* item = dynamic_cast<ComboBoxItem*>(
				stageEditor->GetGeneratedItem(index));
			if (!item) continue;
			const auto itemBounds = item->GetRenderedAbsoluteRectDip();
			popupContainsGeneratedItems = popupContainsGeneratedItems
				&& itemBounds.left >= popupBounds.left - 1.0f
				&& itemBounds.right <= popupBounds.right + 1.0f
				&& itemBounds.top >= popupBounds.top - 1.0f
				&& itemBounds.bottom <= popupBounds.bottom + 1.0f;
		}
		if (dropDownBorder->Background.Kind
				!= cui::drawing::BrushKind::Solid
			|| dropDownBorder->Background.Color.a
				* dropDownBorder->Background.Opacity <= 0.99f
			|| dropDownBorder->BorderBrush.Kind
				!= cui::drawing::BrushKind::Solid
			|| dropDownBorder->BorderBrush.Color.a
				* dropDownBorder->BorderBrush.Opacity <= 0.8f
			|| dropDownBorder->BorderThickness.Left < 1.25f
			|| !popupOverlapsTextCell || !popupContainsGeneratedItems
			|| !hasPopupSnapshot
			|| !popupSnapshot.Overlay
			|| popupPresentationOrder <= coveredPresentationOrder)
			return fail(L"DataGrid ComboBox Popup 未以不透明、有边界的最高层"
				L"覆盖后方文本：overlap="
				+ std::to_wstring(popupOverlapsTextCell)
				+ L"，contains=" + std::to_wstring(
					popupContainsGeneratedItems)
				+ L"，overlay=" + std::to_wstring(popupSnapshot.Overlay)
				+ L"，order=" + std::to_wstring(coveredPresentationOrder)
				+ L"/" + std::to_wstring(popupPresentationOrder) + L"。");

		const int previousStageIndex = stageEditor->GetSelectedIndex();
		int targetStageIndex = -1;
		ComboBoxItem* targetStageItem = nullptr;
		POINT targetStageClientPoint{};
		RECT clientBounds{};
		(void)::GetClientRect(Handle, &clientBounds);
		const auto contentOriginPixels = ContentDipRectToClientPixels(
			D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f));
		const float inputDpiScale = GetDpiScale();
		auto nativePointHitsItem = [&](ComboBoxItem& candidate,
			int clientX, int clientY)
			{
				const int contentX = static_cast<LONG>(clientX / inputDpiScale);
				const int contentY = static_cast<LONG>(
					(clientY - contentOriginPixels.top) / inputDpiScale);
				auto* hit = cui::framework::WindowAccess::HitTestControlAt(
					*this, contentX, contentY);
				for (auto* current = hit; current;
					current = current->GetVisualParent())
					if (current == &candidate) return true;
				return false;
			};
		for (size_t index = 0; index < stageEditor->ItemCount(); ++index)
		{
			if (static_cast<int>(index) == previousStageIndex) continue;
			auto* candidate = dynamic_cast<ComboBoxItem*>(
				stageEditor->GetGeneratedItem(index));
			if (!candidate) continue;
			const auto candidateBounds = candidate->GetRenderedAbsoluteRectDip();
			if (candidateBounds.right <= candidateBounds.left
				|| candidateBounds.bottom <= candidateBounds.top) continue;
			const auto candidatePixels =
				ContentDipRectToClientPixels(candidateBounds);
			const int left = (std::max)(clientBounds.left, candidatePixels.left);
			const int top = (std::max)(clientBounds.top, candidatePixels.top);
			const int right = (std::min)(clientBounds.right, candidatePixels.right);
			const int bottom = (std::min)(clientBounds.bottom, candidatePixels.bottom);
			if (right <= left || bottom <= top) continue;
			POINT candidatePoint{
				left + (right - left) / 2, top + (bottom - top) / 2 };
			bool foundPoint = nativePointHitsItem(
				*candidate, candidatePoint.x, candidatePoint.y);
			for (int y = top; !foundPoint && y < bottom; ++y)
				for (int x = left; x < right; ++x)
					if (nativePointHitsItem(*candidate, x, y))
					{
						candidatePoint = POINT{ x, y };
						foundPoint = true;
						break;
					}
			if (!foundPoint) continue;
			targetStageIndex = static_cast<int>(index);
			targetStageItem = candidate;
			targetStageClientPoint = candidatePoint;
			break;
		}
		if (!targetStageItem)
			return fail(
				L"DataGrid ComboBox Popup 没有可由真实 Window 命中的非当前选项。");
		const ControlWeakReference editorLifetime(stageEditor);
		const ControlWeakReference selectionFaceLifetime(editingSelectionFace);
		const ControlWeakReference stageCellLifetime(stageCell);
		const ControlWeakReference targetStageItemLifetime(targetStageItem);
		const uint64_t selectionRevisionBeforeClick =
			editingSelectionFace->GetPresentationRevisions().Content;
		const LPARAM targetStagePoint = MAKELPARAM(
			targetStageClientPoint.x, targetStageClientPoint.y);
		(void)::SendMessageW(
			Handle, WM_LBUTTONDOWN, MK_LBUTTON, targetStagePoint);
		auto* editorAfterTargetDown = dynamic_cast<ComboBox*>(
			editorLifetime.Get());
		const bool popupOpenAfterTargetDown = editorAfterTargetDown
			&& editorAfterTargetDown->GetIsDropDownOpen();
		auto* capturedAfterTargetDown = GetMouseCaptured();
		const bool targetCapturedAfterDown =
			capturedAfterTargetDown == targetStageItemLifetime.Get();
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0, targetStagePoint);
		// Popup focus restoration must naturally return to the editor; explicitly
		// refocusing here would hide the reported focused-but-empty failure mode.
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid ComboBox 选择后的 retained 帧未稳定。");
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		auto* currentStageCell = firstRow ? firstRow->GetCell(3) : nullptr;
		auto* currentStageEditor = dynamic_cast<ComboBox*>(
			currentStageCell ? currentStageCell->GetEditingElement() : nullptr);
		auto* currentSelectionFace = currentStageEditor
			? dynamic_cast<Label*>(currentStageEditor->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_SelectionBox"))) : nullptr;
		auto* currentEditingChrome = currentStageEditor
			? dynamic_cast<Border*>(currentStageEditor->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_ComboBoxChrome"))) : nullptr;
		PresentationNodeSnapshot editedSelectionSnapshot{};
		const bool hasEditedSelectionSnapshot =
			currentSelectionFace
			&& cui::framework::WindowAccess::TryGetPresentationNodeSnapshot(
				*this, currentSelectionFace, editedSelectionSnapshot);
		const std::wstring selectedStageText = currentStageEditor
			? currentStageEditor->Text : std::wstring{};
		const auto selectedStageForeground = currentSelectionFace
			? currentSelectionFace->Foreground : cui::drawing::Brush{};
		const bool editedSelectionHasVisibleGeometry =
			hasEditedSelectionSnapshot
			&& editedSelectionSnapshot.HasGeometry
			&& editedSelectionSnapshot.RenderedBounds.right
				> editedSelectionSnapshot.RenderedBounds.left
			&& editedSelectionSnapshot.RenderedBounds.bottom
				> editedSelectionSnapshot.RenderedBounds.top;
		if (!currentStageCell || !currentStageEditor || !currentSelectionFace
			|| !currentEditingChrome
			|| currentStageCell != stageCellLifetime.Get()
			|| currentStageEditor != editorLifetime.Get()
			|| currentSelectionFace != selectionFaceLifetime.Get()
			|| currentStageEditor->GetIsDropDownOpen()
			|| !currentStageCell->GetIsEditing()
			|| GetKeyboardFocusedElement() != currentStageEditor
			|| currentStageEditor->GetSelectedIndex() != targetStageIndex
			|| selectedStageText.empty()
			|| selectedStageText == originalStageText
			|| currentSelectionFace->Text != selectedStageText
			|| !currentSelectionFace->GetIsVisible()
			|| selectedStageForeground.Color.a
				* selectedStageForeground.Opacity <= 0.8f
			|| currentSelectionFace->GetActualSizeDip().width <= 0.0f
			|| currentSelectionFace->GetActualSizeDip().height <= 0.0f
			|| currentSelectionFace->GetPresentationRevisions().Content
				<= selectionRevisionBeforeClick
			|| currentEditingChrome->BorderThickness != Thickness(1.0f)
			|| !hasEditedSelectionSnapshot
			|| !editedSelectionSnapshot.HasPresented
			|| !editedSelectionHasVisibleGeometry
			|| editedSelectionSnapshot.ContentDirty)
			return fail(L"DataGrid ComboBox 选择后在焦点内未立即显示文本：text="
				+ selectedStageText + L"，face=" + (currentSelectionFace
					? currentSelectionFace->Text : std::wstring{})
				+ L"，focus=" + std::to_wstring(
					GetKeyboardFocusedElement() == currentStageEditor)
				+ L"，open=" + std::to_wstring(
					currentStageEditor && currentStageEditor->GetIsDropDownOpen())
				+ L"，index=" + std::to_wstring(previousStageIndex)
				+ L"/" + std::to_wstring(targetStageIndex) + L"/"
				+ std::to_wstring(currentStageEditor
					? currentStageEditor->GetSelectedIndex() : -1)
				+ L"，down=" + std::to_wstring(popupOpenAfterTargetDown)
				+ L"/" + std::to_wstring(
					targetCapturedAfterDown)
				+ L"，editing=" + std::to_wstring(
					currentStageCell && currentStageCell->GetIsEditing())
				+ L"，size=" + std::to_wstring(
					currentSelectionFace
						? currentSelectionFace->GetActualSizeDip().width : 0.0f)
				+ L"，revision=" + std::to_wstring(selectionRevisionBeforeClick)
				+ L"→" + std::to_wstring(
					currentSelectionFace
						? currentSelectionFace->GetPresentationRevisions().Content : 0)
				+ L"，border=" + std::to_wstring(
					currentEditingChrome
						? currentEditingChrome->BorderThickness.Left : 0.0f)
				+ L"，snapshot=" + std::to_wstring(hasEditedSelectionSnapshot)
				+ L"/" + std::to_wstring(
					editedSelectionSnapshot.HasPresented)
				+ L"/" + std::to_wstring(
					editedSelectionSnapshot.ContentDirty)
				+ L"，editorIdentity=" + std::to_wstring(
					currentStageEditor == editorLifetime.Get())
				+ L"，faceIdentity=" + std::to_wstring(
					currentSelectionFace == selectionFaceLifetime.Get()) + L"。");
		if (!dataGrid->CancelEdit())
			return fail(L"DataGrid ComboBox 编辑验证无法结束选择。");
		dataGrid->UnselectAllCells();
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		customerCell = firstRow ? firstRow->GetCell(1) : nullptr;
		stageCell = firstRow ? firstRow->GetCell(3) : nullptr;
		stageDisplay = stageCell ? dynamic_cast<ComboBox*>(
			stageCell->GetVisualContent()) : nullptr;
		if (!stageDisplay || stageDisplay->Text != selectedStageText)
			return fail(L"DataGrid ComboBox 结束编辑后未保留已选显示投影。");

		const auto cellBounds = customerCell->GetAbsoluteBoundsDip();
		const auto cellPixels = ContentDipRectToClientPixels(cellBounds);
		const LPARAM cellPoint = MAKELPARAM(
			(cellPixels.left + cellPixels.right) / 2,
			(cellPixels.top + cellPixels.bottom) / 2);
		(void)::SendMessageW(Handle, WM_LBUTTONDOWN, MK_LBUTTON, cellPoint);
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0, cellPoint);
		// A viewport/layout commit may recycle realized containers.  CurrentCell
		// is the stable identity contract; reacquire the current realization
		// before validating hit-test ancestry or sending the edit key.
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		customerCell = firstRow ? firstRow->GetCell(1) : nullptr;
		auto* hit = cui::framework::WindowAccess::HitTestControlAt(
			*this,
			static_cast<int>((cellBounds.left + cellBounds.right) * 0.5f),
			static_cast<int>((cellBounds.top + cellBounds.bottom) * 0.5f));
		bool hitBelongsToCell = false;
		for (auto* current = hit; current; current = current->GetVisualParent())
			if (current == customerCell)
			{
				hitBelongsToCell = true;
				break;
			}
		const auto currentCell = dataGrid->GetCurrentCell();
		if (!customerCell || !hitBelongsToCell || !currentCell.IsValid()
			|| currentCell.RowIndex != 0 || currentCell.ColumnIndex != 1
			|| firstRow->GetIsSelected() || dataGrid->GetSelectedIndex() != -1
			|| !customerCell->GetIsSelected()
			|| !dataGrid->IsCellSelected(0, 1)
			|| dataGrid->GetSelectedCells().size() != 1)
			return fail(
				L"DataGrid 真实 Window hit-test 未建立独立 Cell 选择：hit="
				+ std::to_wstring(hit
					? static_cast<int>(hit->Type()) : -1)
				+ L"，belongs=" + std::to_wstring(hitBelongsToCell)
				+ L"，current=" + std::to_wstring(currentCell.IsValid()) + L"/"
				+ std::to_wstring(currentCell.RowIndex) + L"/"
				+ std::to_wstring(currentCell.ColumnIndex)
				+ L"，row=" + std::to_wstring(firstRow->GetIsSelected())
				+ L"，cell=" + std::to_wstring(customerCell->GetIsSelected())
				+ L"，selectedIndex="
				+ std::to_wstring(dataGrid->GetSelectedIndex())
				+ L"，selectedCells="
				+ std::to_wstring(dataGrid->GetSelectedCells().size()) + L"。");
		// WPF treats WM_*DBLCLK as the second left-button press. The already
		// focused/selected editable cell must therefore enter edit, without
		// converting CellOrRowHeader selection into a selected row.
		(void)::SendMessageW(
			Handle, WM_LBUTTONDBLCLK, MK_LBUTTON, cellPoint);
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0, cellPoint);
		auto* doubleClickEditor = dynamic_cast<TextBox*>(
			customerCell->GetEditingElement());
		if (!customerCell->GetIsEditing() || !doubleClickEditor
			|| firstRow->GetIsSelected() || dataGrid->GetSelectedIndex() != -1)
			return fail(
				L"DataGrid 可编辑单元格双击未进入编辑或错误提升为整行选择。");
		BindingValue doubleClickCustomer;
		if (!currentCell.Item
			|| !currentCell.Item.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Customer"),
				doubleClickCustomer)
			|| doubleClickCustomer.ToString().empty()
			|| doubleClickEditor->Text != doubleClickCustomer.ToString())
			return fail(
				L"DataGrid 双击创建的 TextBox 未在同一输入回合投影 Customer：source="
				+ doubleClickCustomer.ToString() + L"，editor="
				+ (doubleClickEditor
					? doubleClickEditor->Text : std::wstring(L"<null>")) + L"。");
		(void)doubleClickEditor->ApplyTemplate();
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid 双击 TextBox 首帧未完成 retained 绘制。");
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		customerCell = firstRow ? firstRow->GetCell(1) : nullptr;
		doubleClickEditor = customerCell ? dynamic_cast<TextBox*>(
			customerCell->GetEditingElement()) : nullptr;
		auto* doubleClickContentHost = doubleClickEditor
			? doubleClickEditor->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_ContentHost")) : nullptr;
		auto* doubleClickTemplateRoot = doubleClickEditor
			? dynamic_cast<Border*>(cui::framework::TemplateAccess::
				GetTemplateRoot(*doubleClickEditor)) : nullptr;
		const auto doubleClickForeground = doubleClickEditor
			? doubleClickEditor->GetComputedForegroundBrush()
			: cui::drawing::NoBrush();
		const auto doubleClickRootBackground = doubleClickTemplateRoot
			? doubleClickTemplateRoot->GetComputedBackgroundBrush()
			: cui::drawing::NoBrush();
		const auto doubleClickEditorSize = doubleClickEditor
			? doubleClickEditor->GetActualSizeDip() : cui::core::Size{};
		const auto doubleClickHostSize = doubleClickContentHost
			? doubleClickContentHost->GetActualSizeDip() : cui::core::Size{};
		const auto doubleClickCellRect = customerCell
			? customerCell->GetRenderedAbsoluteRectDip() : D2D1_RECT_F{};
		const auto doubleClickEditorRect = doubleClickEditor
			? doubleClickEditor->GetRenderedAbsoluteRectDip() : D2D1_RECT_F{};
		PresentationNodeSnapshot doubleClickEditorSnapshot{};
		const bool hasDoubleClickEditorSnapshot = doubleClickEditor
			&& cui::framework::WindowAccess::TryGetPresentationNodeSnapshot(
				*this, doubleClickEditor, doubleClickEditorSnapshot);
		constexpr float doubleClickBoundsTolerance = 0.75f;
		if (!customerCell || !doubleClickEditor || !doubleClickContentHost
			|| !doubleClickTemplateRoot
			|| doubleClickEditor->Text != doubleClickCustomer.ToString()
			|| !doubleClickEditor->GetIsVisible()
			|| doubleClickEditorSize.width <= 0.0f
			|| doubleClickEditorSize.height <= 0.0f
			|| doubleClickHostSize.width <= 0.0f
			|| doubleClickHostSize.height <= 0.0f
			|| doubleClickForeground.Kind != cui::drawing::BrushKind::Solid
			|| doubleClickForeground.Color.a * doubleClickForeground.Opacity <= 0.8f
			|| doubleClickRootBackground.Kind
				!= cui::drawing::BrushKind::Solid
			|| doubleClickRootBackground.Color.a
				* doubleClickRootBackground.Opacity > 0.001f
			|| doubleClickEditorRect.left
				< doubleClickCellRect.left - doubleClickBoundsTolerance
			|| doubleClickEditorRect.top
				< doubleClickCellRect.top - doubleClickBoundsTolerance
			|| doubleClickEditorRect.right
				> doubleClickCellRect.right + doubleClickBoundsTolerance
			|| doubleClickEditorRect.bottom
				> doubleClickCellRect.bottom + doubleClickBoundsTolerance
			|| !hasDoubleClickEditorSnapshot
			|| !doubleClickEditorSnapshot.HasPresented
			|| !doubleClickEditorSnapshot.HasGeometry
			|| doubleClickEditorSnapshot.ContentDirty
			|| doubleClickEditorSnapshot.GeometryDirty
			|| doubleClickEditorSnapshot.CompositionDirty)
			return fail(
				L"DataGrid 双击 TextBox 首帧文本被模板遮盖或未进入 retained 场景：text="
				+ (doubleClickEditor
					? doubleClickEditor->Text : std::wstring(L"<null>"))
				+ L"，host=" + std::to_wstring(doubleClickContentHost != nullptr)
				+ L"，root=" + std::to_wstring(doubleClickTemplateRoot != nullptr)
				+ L"，background=" + std::to_wstring(
					doubleClickRootBackground.Color.a
						* doubleClickRootBackground.Opacity)
				+ L"，foreground=" + std::to_wstring(
					doubleClickForeground.Color.a
						* doubleClickForeground.Opacity)
				+ L"，editorSize=" + std::to_wstring(doubleClickEditorSize.width)
				+ L"x" + std::to_wstring(doubleClickEditorSize.height)
				+ L"，hostSize=" + std::to_wstring(doubleClickHostSize.width)
				+ L"x" + std::to_wstring(doubleClickHostSize.height)
				+ L"，snapshot=" + std::to_wstring(
					hasDoubleClickEditorSnapshot)
				+ L"/" + std::to_wstring(doubleClickEditorSnapshot.HasPresented)
				+ L"/" + std::to_wstring(doubleClickEditorSnapshot.HasGeometry)
				+ L"/" + std::to_wstring(
					doubleClickEditorSnapshot.HasDrawingCommands)
				+ L"，dirty=" + std::to_wstring(
					doubleClickEditorSnapshot.ContentDirty)
				+ L"/" + std::to_wstring(
					doubleClickEditorSnapshot.GeometryDirty)
				+ L"/" + std::to_wstring(
					doubleClickEditorSnapshot.CompositionDirty) + L"。");
		(void)::SendMessageW(Handle, WM_KEYDOWN, VK_ESCAPE, 0);
		(void)::SendMessageW(Handle, WM_KEYUP, VK_ESCAPE, 0);
		if (customerCell->GetIsEditing())
			return fail(L"DataGrid 双击编辑验证无法取消编辑。");
		(void)::SendMessageW(Handle, WM_KEYDOWN, VK_F2, 0);
		(void)::SendMessageW(Handle, WM_KEYUP, VK_F2, 0);
		auto* editor = dynamic_cast<TextBox*>(customerCell->GetEditingElement());
		if (!customerCell->GetIsEditing() || !editor)
			return fail(L"DataGrid F2 真实键盘输入未进入 TextBox 编辑态。");
		(void)editor->ApplyTemplate();
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid TextBox 编辑模板未完成 Production 布局。");
		const auto contentHostToken = MakeTemplatePartToken(L"PART_ContentHost");
		const auto deleteButtonToken = MakeTemplatePartToken(L"DeleteButton");
		const auto contentBorderToken = MakeTemplatePartToken(L"ContentBorder");
		auto* contentHost = editor->FindDeclarativeTemplatePart(contentHostToken);
		auto* deleteButton = editor->FindDeclarativeTemplatePart(deleteButtonToken);
		auto* contentBorder = editor->FindDeclarativeTemplatePart(contentBorderToken);
		if (!contentHost || deleteButton
			|| editor->BorderThickness != Thickness{}
			|| editor->Padding != Thickness{}
			|| editor->VerticalContentAlignment != VerticalAlignment::Center
			|| editor->Background.Kind != cui::drawing::BrushKind::Solid
			|| editor->Background.Color.a > 0.001f
			|| editor->SelectionBrush.Kind
				!= cui::drawing::BrushKind::Solid
			|| editor->SelectionBrush.Color.a < 0.8f
			|| std::abs(editor->SelectionOpacity - 1.0) > 0.001
			|| editor->SelectionTextBrush.Kind
				!= cui::drawing::BrushKind::Solid
			|| editor->SelectionTextBrush.Color.r < 0.9f
			|| (contentBorder && contentBorder->BorderThickness != Thickness{}))
			return fail(
				L"DataGrid TextBox 仍在使用普通输入框圆角/清除按钮样式：host="
				+ std::to_wstring(contentHost != nullptr)
				+ L"，delete=" + std::to_wstring(deleteButton != nullptr)
				+ L"，style="
				+ cui::framework::StyleAccess::ResourceKey(*editor)
				+ L"，automatic=" + std::to_wstring(
					cui::framework::StyleAccess::ResourceKeyIsAutomatic(*editor))
				+ L"，visibleRules=" + std::to_wstring(
					cui::framework::StyleAccess::HasVisibleStyleRules(*editor))
				+ L"，theme=" + std::to_wstring(static_cast<bool>(
					cui::framework::StyleAccess::Theme(*editor))) + L"。");
		const auto editingCellRect = customerCell->GetRenderedAbsoluteRectDip();
		const auto editorRect = editor->GetRenderedAbsoluteRectDip();
		D2D1_RECT_F editorCaretRect{};
		if (!cui::framework::InputAccess::ResolveTextInputCaretRect(
			*editor, editorCaretRect)
			|| editorCaretRect.left <= editorRect.left + 4.0f)
			return fail(
				L"DataGrid F2 首帧保留了零宽布局产生的陈旧文本滚动偏移。");
		constexpr float editorBoundsTolerance = 0.75f;
		constexpr float cellChromeAllowance = 1.1f;
		const auto editingCellPadding = customerCell->Padding;
		if (editorRect.left < editingCellRect.left - editorBoundsTolerance
			|| editorRect.top < editingCellRect.top - editorBoundsTolerance
			|| editorRect.right > editingCellRect.right + editorBoundsTolerance
			|| editorRect.bottom > editingCellRect.bottom + editorBoundsTolerance
			|| editorRect.left > editingCellRect.left + cellChromeAllowance
				+ editingCellPadding.Left
			|| editorRect.top > editingCellRect.top + cellChromeAllowance
				+ editingCellPadding.Top
			|| editorRect.right < editingCellRect.right - cellChromeAllowance
				- editingCellPadding.Right
			|| editorRect.bottom < editingCellRect.bottom - cellChromeAllowance
				- editingCellPadding.Bottom)
			return fail(
				L"DataGrid TextBox 编辑器未填满单元格：cell="
				+ std::to_wstring(editingCellRect.left) + L"/"
				+ std::to_wstring(editingCellRect.top) + L"/"
				+ std::to_wstring(editingCellRect.right) + L"/"
				+ std::to_wstring(editingCellRect.bottom) + L"，editor="
				+ std::to_wstring(editorRect.left) + L"/"
				+ std::to_wstring(editorRect.top) + L"/"
				+ std::to_wstring(editorRect.right) + L"/"
				+ std::to_wstring(editorRect.bottom) + L"。");
		// Exercise the production HWND text path with a compact burst.  The F2
		// activation selected the old value, so the first character replaces it;
		// OnValidation must keep the source untouched until a commit.
		const std::wstring rapidInput = L"Rapid-0123456789-Input";
		BindingValue customerBeforeBurst;
		if (!currentCell.Item
			|| !currentCell.Item.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Customer"), customerBeforeBurst))
			return fail(L"DataGrid 快速输入验证无法读取编辑前源值。");
		for (const wchar_t character : rapidInput)
			(void)::SendMessageW(
				Handle, WM_CHAR, static_cast<WPARAM>(character), 0);
		BindingValue deferredCustomer;
		if (editor->Text != rapidInput
			|| !currentCell.Item.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Customer"), deferredCustomer)
			|| deferredCustomer.ToString() != customerBeforeBurst.ToString())
			return fail(
				L"DataGrid 快速 WM_CHAR 输入未保持编辑器即时更新/提交前延迟写回。");
		(void)::SendMessageW(Handle, WM_KEYDOWN, VK_ESCAPE, 0);
		(void)::SendMessageW(Handle, WM_KEYUP, VK_ESCAPE, 0);
		if (customerCell->GetIsEditing())
			return fail(L"DataGrid Esc 真实键盘输入未取消单元格编辑。");

		// The amount display owns the currency StringFormat, while its editing
		// template binds the raw Int64. Enter must therefore commit and leave edit
		// mode instead of being trapped by conversion of text such as "¥ 86,400".
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		auto* amountCell = firstRow ? firstRow->GetCell(5) : nullptr;
		const auto amountItem = firstRow
			? firstRow->GetItem() : BindingSourceReference{};
		BindingValue originalAmount;
		long long originalAmountValue = 0;
		if (!amountItem || !amountItem.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Amount"), originalAmount)
			|| !originalAmount.TryGetInt64(originalAmountValue))
			return fail(L"DataGrid 金额模板无法读取原始 Int64 值。");
		if (!amountCell || !dataGrid->SetCurrentCell(0, 5)
			|| !dataGrid->BeginEdit())
			return fail(L"DataGrid 金额模板无法进入编辑态。");
		auto* amountEditor = dynamic_cast<TextBox*>(
			amountCell->GetEditingElement());
		if (!amountEditor
			|| amountEditor->Text != std::to_wstring(originalAmountValue))
			return fail(L"DataGrid 金额编辑态仍包含货币格式文本："
				+ (amountEditor ? amountEditor->Text : L"<null>") + L"。");
		const long long enterAmountValue = originalAmountValue + 1000;
		amountEditor->SelectAll();
		amountEditor->InsertText(std::to_wstring(enterAmountValue));
		(void)::SendMessageW(Handle, WM_KEYDOWN, VK_RETURN, 0);
		(void)::SendMessageW(Handle, WM_KEYUP, VK_RETURN, 0);
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		amountCell = firstRow ? firstRow->GetCell(5) : nullptr;
		BindingValue committedAmount;
		long long committedAmountValue = 0;
		if (!amountCell || amountCell->GetIsEditing()
			|| !amountItem || !amountItem.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Amount"), committedAmount)
			|| !committedAmount.TryGetInt64(committedAmountValue)
			|| committedAmountValue != enterAmountValue)
			return fail(L"DataGrid 金额编辑按 Enter 未提交退出并写回 Int64。");
		if (!dataGrid->SetCurrentCell(0, 5) || !dataGrid->BeginEdit())
			return fail(L"DataGrid 金额模板无法再次进入鼠标提交验证。");
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		amountCell = firstRow ? firstRow->GetCell(5) : nullptr;
		amountEditor = amountCell
			? dynamic_cast<TextBox*>(amountCell->GetEditingElement()) : nullptr;
		if (!amountEditor
			|| amountEditor->Text != std::to_wstring(enterAmountValue))
			return fail(L"DataGrid 金额再次编辑未读取已提交的原始 Int64。");
		const long long pointerAmountValue = originalAmountValue + 2000;
		amountEditor->SelectAll();
		amountEditor->InsertText(std::to_wstring(pointerAmountValue));

		// The demo deliberately uses WPF's interactive-template pattern for a
		// one-click boolean cell. The column remains read-only so the embedded
		// CheckBox owns the press instead of opening a DataGrid edit transaction.
		// Press activation also survives the cell taking keyboard focus during the
		// same routed input transaction.
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		auto* paidCell = firstRow ? firstRow->GetCell(6) : nullptr;
		auto* paidCheck = paidCell
			? dynamic_cast<CheckBox*>(paidCell->GetVisualContent()) : nullptr;
		const auto paidItem = firstRow
			? firstRow->GetItem() : BindingSourceReference{};
		BindingValue paidBefore;
		bool paidBeforeValue = false;
		if (!paidCell || !paidCheck || !paidItem
			|| !paidItem.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Paid"), paidBefore)
			|| !paidBefore.TryGetBool(paidBeforeValue))
			return fail(L"DataGrid 一击 CheckBox 模板未生成有效绑定元素。");
		const auto paidBounds = paidCheck->GetRenderedAbsoluteRectDip();
		const auto paidPixels = ContentDipRectToClientPixels(paidBounds);
		const LPARAM paidPoint = MAKELPARAM(
			(paidPixels.left + paidPixels.right) / 2,
			(paidPixels.top + paidPixels.bottom) / 2);
		(void)::SendMessageW(Handle, WM_LBUTTONDOWN, MK_LBUTTON, paidPoint);
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0, paidPoint);
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		amountCell = firstRow ? firstRow->GetCell(5) : nullptr;
		paidCell = firstRow ? firstRow->GetCell(6) : nullptr;
		paidCheck = paidCell
			? dynamic_cast<CheckBox*>(paidCell->GetVisualContent()) : nullptr;
		BindingValue paidAfter;
		bool paidAfterValue = paidBeforeValue;
		BindingValue pointerCommittedAmount;
		long long pointerCommittedAmountValue = 0;
		const auto paidCurrent = dataGrid->GetCurrentCell();
		if (!amountCell || amountCell->GetIsEditing()
			|| !amountItem.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Amount"), pointerCommittedAmount)
			|| !pointerCommittedAmount.TryGetInt64(pointerCommittedAmountValue)
			|| pointerCommittedAmountValue != pointerAmountValue
			|| !paidCell || !paidCheck || !paidItem.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Paid"), paidAfter)
			|| !paidAfter.TryGetBool(paidAfterValue)
			|| paidAfterValue == paidBeforeValue
			|| paidCell->GetIsEditing() || !paidCurrent.IsValid()
			|| paidCurrent.RowIndex != 0 || paidCurrent.ColumnIndex != 6
			|| !paidCell->GetIsSelected())
			return fail(
				L"DataGrid 点击已付款未提交金额，或方框首次真实单击未同时选择并切换："
				L"amount=" + std::to_wstring(pointerCommittedAmountValue)
				+ L"，amountEditing="
				+ std::to_wstring(amountCell && amountCell->GetIsEditing())
				+ L"，before=" + std::to_wstring(paidBeforeValue)
				+ L"，after=" + std::to_wstring(paidAfterValue)
				+ L"，checked=" + std::to_wstring(
					paidCheck && paidCheck->IsChecked)
				+ L"，editing=" + std::to_wstring(
					paidCell && paidCell->GetIsEditing())
				+ L"，selected=" + std::to_wstring(
					paidCell && paidCell->GetIsSelected())
				+ L"，current=" + std::to_wstring(paidCurrent.IsValid())
				+ L"/" + std::to_wstring(paidCurrent.RowIndex)
				+ L"/" + std::to_wstring(paidCurrent.ColumnIndex) + L"。");

		// Start with neither retained damage nor an older coalesced token, so the
		// turn consumed below can only have been scheduled by this resize gesture.
		VisibleWindowScope dataGridResizeVisibility(Handle);
		// SW_SHOWNOACTIVATE deliberately leaves this hidden-HWND smoke inactive.
		// Establish native focus before synthesizing a captured drag; otherwise
		// USER32 may issue WM_CANCELMODE and correctly roll the resize back.
		(void)::SetActiveWindow(Handle);
		(void)::SetFocus(Handle);
		if (::GetActiveWindow() != Handle || ::GetFocus() != Handle)
			return fail(L"DataGrid 列宽连续输入验证无法激活原生窗口。");
		if (!drainPresentationWork())
			return fail(L"DataGrid 列宽连续输入验证无法排空旧绘制。");
		clearPresentationTurns();
		firstHeader = headerPresenter
			? dynamic_cast<DataGridColumnHeader*>(
				headerPresenter->GetVisualChild(0)) : nullptr;
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		if (!firstHeader || !firstRow)
			return fail(L"DataGrid 列宽连续输入验证缺少稳定容器。");
		const auto headerBounds = firstHeader->GetAbsoluteBoundsDip();
		const auto headerPixels = ContentDipRectToClientPixels(headerBounds);
		auto* headerBeforeResize = firstHeader;
		auto* rowBeforeResize = firstRow;
		auto* firstColumn = firstHeader->GetColumn();
		const double widthBeforeDrag = firstColumn
			&& firstColumn->GetWidth().UnitType == DataGridLengthUnitType::Pixel
			? firstColumn->GetWidth().Value
			: static_cast<double>(headerBounds.right - headerBounds.left);
		const int resizeStartX = (std::max)(
			headerPixels.left, headerPixels.right - 2);
		const int resizeY = (headerPixels.top + headerPixels.bottom) / 2;
		const LPARAM resizeStart = MAKELPARAM(resizeStartX, resizeY);
		const LPARAM resizeMove = MAKELPARAM(resizeStartX + 24, resizeY);
		const HCURSOR sizeCursor = ::LoadCursorW(nullptr, IDC_SIZEWE);
		int resizeMoveCount = 0;
		int resizeMoveLocalX = (std::numeric_limits<int>::min)();
		auto resizeMoveConnection = firstHeader->OnMouseMove.SubscribeHandledEventsToo(
			[&](Control*, MouseEventArgs& args)
			{
				++resizeMoveCount;
				resizeMoveLocalX = args.X;
			});
		(void)::SendMessageW(Handle, WM_MOUSEMOVE, 0, resizeStart);
		const bool resizeHoverCursor = ::GetCursor() == sizeCursor;
		(void)::SendMessageW(
			Handle, WM_LBUTTONDOWN, MK_LBUTTON, resizeStart);
		const bool resizeCaptured = firstHeader->IsMouseCaptured();
		const bool resizePressSuppressed = !firstHeader->IsPressed;
		// A press may invalidate focus/chrome. Consume that exact turn first so the
		// baseline below isolates frames requested by the following move burst.
		clearPresentationTurns();
		const bool resizeCaptureSurvivedPressPresentation =
			GetMouseCaptured() == headerBeforeResize;
		const auto committedBeforeResizeBurst =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		bool resizeContainersStable = true;
		bool resizeCaptureCursor = true;
		bool resizePresentationTurnsDispatched = true;
		bool resizeFramesFollowedPointer = true;
		bool resizeGeometryFollowedPointer = true;
		bool resizeFramesStayedLocal = true;
		bool resizeFramesDrainedDamage = true;
		RECT resizeFrameDirty{};
		bool resizeFrameWasFull = true;
		auto committedDuringResizeBurst = committedBeforeResizeBurst;
		const double resizeDipPerPixel = 1.0
			/ static_cast<double>((std::max)(0.001f, GetDpiScale()));
		for (int burstEnd = 6; burstEnd <= 24; burstEnd += 6)
		{
			const auto committedBeforeBurst =
				cui::framework::WindowAccess::
					PresentationCommittedFrameCount(*this);
			for (int step = burstEnd - 5; step <= burstEnd; ++step)
			{
				const LPARAM resizeStep = MAKELPARAM(
					resizeStartX + step, resizeY);
				(void)::SendMessageW(
					Handle, WM_MOUSEMOVE, MK_LBUTTON, resizeStep);
				resizeContainersStable = resizeContainersStable
					&& headerPresenter->GetVisualChild(0)
						== headerBeforeResize
					&& dataGrid->GetGeneratedItem(0) == rowBeforeResize;
				resizeCaptureCursor = resizeCaptureCursor
					&& ::GetCursor() == sizeCursor;
			}
			(void)dispatchPresentationTurn();
			auto committedAfterDispatch =
				cui::framework::WindowAccess::
					PresentationCommittedFrameCount(*this);
			// A nested USER32/DWM call is allowed to consume the posted private
			// token while SendMessage is routing the burst.  If neither that path
			// nor the explicit dequeue committed the frame, service the ordinary
			// WM_PAINT transaction synchronously; frame/geometry/damage assertions
			// below remain the actual production contract.
			if (committedAfterDispatch <= committedBeforeBurst
				&& cui::framework::WindowAccess::HasPendingRenderWork(*this))
			{
				(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
				committedAfterDispatch =
					cui::framework::WindowAccess::
						PresentationCommittedFrameCount(*this);
			}
			resizePresentationTurnsDispatched =
				(committedAfterDispatch > committedBeforeBurst)
				&& resizePresentationTurnsDispatched;
			const bool hasResizeFrame =
				cui::framework::WindowAccess::TryGetLastRenderDirtyRect(
					*this, resizeFrameDirty, resizeFrameWasFull);
			resizeFramesStayedLocal = resizeFramesStayedLocal
				&& hasResizeFrame && !resizeFrameWasFull;
			resizeFramesDrainedDamage = resizeFramesDrainedDamage
				&& !cui::framework::WindowAccess::
					HasPendingPresentationDamage(*this);
			committedDuringResizeBurst =
				cui::framework::WindowAccess::
					PresentationCommittedFrameCount(*this);
			resizeFramesFollowedPointer = resizeFramesFollowedPointer
				&& committedDuringResizeBurst > committedBeforeBurst;
			firstHeader = headerPresenter
				? dynamic_cast<DataGridColumnHeader*>(
					headerPresenter->GetVisualChild(0)) : nullptr;
			firstRow = dynamic_cast<DataGridRow*>(
				dataGrid->GetGeneratedItem(0));
			auto* firstCell = firstRow ? firstRow->GetCell(0) : nullptr;
			const double expectedFrameWidth = widthBeforeDrag
				+ static_cast<double>(burstEnd) * resizeDipPerPixel;
			resizeGeometryFollowedPointer = resizeGeometryFollowedPointer
				&& firstHeader == headerBeforeResize
				&& firstRow == rowBeforeResize
				&& firstCell
				&& std::abs(firstHeader->GetActualSizeDip().width
					- expectedFrameWidth) <= 3.0
				&& std::abs(firstCell->GetActualSizeDip().width
					- expectedFrameWidth) <= 3.0;
		}
		// SendMessage keeps every burst inside the test. Each serviced presentation
		// must commit the latest header and cell geometry while capture is still active;
		// PointerUp must not be what finally makes a resize visible.
		const bool resizeCaptureSurvivedPresentation =
			GetMouseCaptured() == headerBeforeResize;
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0, resizeMove);
		RequestLayout();
		UpdateLayout();
		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		firstHeader = headerPresenter
			? dynamic_cast<DataGridColumnHeader*>(
				headerPresenter->GetVisualChild(0)) : nullptr;
		firstColumn = firstHeader ? firstHeader->GetColumn() : nullptr;
		const double expectedResizeDelta = 24.0
			/ static_cast<double>((std::max)(0.001f, GetDpiScale()));
		const double widthAfterDrag = firstColumn
			? firstColumn->GetWidth().Value : -1.0;
		if (!firstHeader || !firstColumn || !resizeHoverCursor
			|| !resizeCaptured || !resizeCaptureCursor
			|| !resizePressSuppressed
			|| !resizeContainersStable
			|| !resizePresentationTurnsDispatched
			|| !resizeFramesFollowedPointer
			|| !resizeGeometryFollowedPointer
			|| !resizeFramesStayedLocal
			|| !resizeFramesDrainedDamage
			|| !resizeCaptureSurvivedPressPresentation
			|| !resizeCaptureSurvivedPresentation
			|| committedDuringResizeBurst <= committedBeforeResizeBurst
			|| firstHeader != headerBeforeResize || firstRow != rowBeforeResize
			|| firstColumn->GetWidth().UnitType
				!= DataGridLengthUnitType::Pixel
			|| std::abs(widthAfterDrag
				- (widthBeforeDrag + expectedResizeDelta)) > 3.0)
			return fail(
				L"DataGrid 表头真实指针拖拽未按 DPI 坐标原位调整列宽："
				L"header=" + std::to_wstring(firstHeader != nullptr)
				+ L"/" + std::to_wstring(firstHeader == headerBeforeResize)
				+ L"，row=" + std::to_wstring(firstRow == rowBeforeResize)
				+ L"，cursor=" + std::to_wstring(resizeHoverCursor)
				+ L"/" + std::to_wstring(resizeCaptureCursor)
				+ L"，stable=" + std::to_wstring(resizeContainersStable)
				+ L"，capture=" + std::to_wstring(resizeCaptured)
				+ L"/"
				+ std::to_wstring(resizeCaptureSurvivedPressPresentation)
				+ L"/" + std::to_wstring(resizeCaptureSurvivedPresentation)
				+ L"，pressed=" + std::to_wstring(!resizePressSuppressed)
				+ L"，dispatch="
				+ std::to_wstring(resizePresentationTurnsDispatched)
				+ L"，follow=" + std::to_wstring(resizeFramesFollowedPointer)
				+ L"/" + std::to_wstring(resizeGeometryFollowedPointer)
				+ L"，local=" + std::to_wstring(resizeFramesStayedLocal)
				+ L"，drained=" + std::to_wstring(resizeFramesDrainedDamage)
				+ L"/" + std::to_wstring(resizeFrameWasFull)
				+ L"/dirty(" + std::to_wstring(resizeFrameDirty.left)
				+ L"," + std::to_wstring(resizeFrameDirty.top)
				+ L"," + std::to_wstring(resizeFrameDirty.right)
				+ L"," + std::to_wstring(resizeFrameDirty.bottom) + L")"
				+ L"，committed="
				+ std::to_wstring(committedBeforeResizeBurst) + L"→"
				+ std::to_wstring(committedDuringResizeBurst)
				+ L"，move=" + std::to_wstring(resizeMoveCount)
				+ L"/" + std::to_wstring(resizeMoveLocalX)
				+ L"，unit=" + std::to_wstring(firstColumn
					? static_cast<int>(firstColumn->GetWidth().UnitType) : -1)
				+ L"，before=" + std::to_wstring(widthBeforeDrag)
				+ L"，after=" + std::to_wstring(widthAfterDrag)
				+ L"，delta=" + std::to_wstring(expectedResizeDelta) + L"。");
		dataGridResizeVisibility.Restore();

		// Use Amount instead of OrderNo here.  The source is already ordered by
		// OrderNo, so its first ascending click is a zero-move fast path and used
		// to hide the former per-Move sort cost in this real HWND smoke.
		auto* sortedHeader = headerPresenter
			? headerPresenter->GetHeader(5) : nullptr;
		if (!sortedHeader || !sortedHeader->GetColumn())
			return fail(L"DataGrid 金额排序列头未生成。");
		const auto sortedHeaderBounds = sortedHeader->GetAbsoluteBoundsDip();
		const auto sortedHeaderPixels =
			ContentDipRectToClientPixels(sortedHeaderBounds);
		const LPARAM headerPoint = MAKELPARAM(
			(sortedHeaderPixels.left + sortedHeaderPixels.right) / 2,
			(sortedHeaderPixels.top + sortedHeaderPixels.bottom) / 2);
		(void)::SendMessageW(Handle, WM_LBUTTONDOWN, MK_LBUTTON, headerPoint);
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0, headerPoint);
		if (sortedHeader->GetColumn()->GetSortDirection()
			!= CollectionSortDirection::Ascending)
			return fail(L"DataGrid 金额表头真实指针输入未触发升序排序。");
		// The WPF-style refresh remaps retained rows atomically and intentionally
		// preserves the old item as scroll anchor, which need not remain logical
		// row zero. Bring the smoke's next target back into view and commit the
		// presentation turn before sending another native pointer sequence.
		auto* dataScroll = dynamic_cast<ScrollViewer*>(
			dataGrid->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_ScrollViewer")));
		if (dataScroll)
			dataScroll->ScrollToVerticalOffset(0.0);
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid 排序后未完成稳定 retained 绘制。");

		firstRow = dynamic_cast<DataGridRow*>(dataGrid->GetGeneratedItem(0));
		BindingSourceReference sortedFirstItem;
		BindingValue sortedAmount;
		long long sortedAmountValue = 0;
		if (!firstRow || !firstRow->GetItem()
			|| !dataGrid->GetItemsSource()
			|| !dataGrid->GetItemsSource().Get()->TryGetItem(0, sortedFirstItem)
			|| firstRow->GetItem().Shared() != sortedFirstItem.Shared()
			|| !firstRow->GetItem().Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Amount"), sortedAmount)
			|| !sortedAmount.TryGetInt64(sortedAmountValue))
			return fail(
				L"DataGrid 金额排序后首行未原位映射到视图：value="
				+ std::to_wstring(sortedAmountValue) + L"。");
		// GroupDescription remains the primary projection: SortDescription orders
		// items numerically inside each region instead of flattening all groups into
		// one global amount sequence.  Validate every adjacent in-group pair so an
		// Int64/string comparison regression cannot hide behind the singleton first
		// group (东北).
		std::wstring previousRegion;
		long long previousAmount = 0;
		bool sawInGroupPair = false;
		for (size_t index = 0;
			index < dataGrid->GetItemsSource().Get()->Count(); ++index)
		{
			BindingSourceReference item;
			BindingValue regionValue;
			BindingValue amountValue;
			long long amount = 0;
			if (!dataGrid->GetItemsSource().Get()->TryGetItem(index, item)
				|| !item
				|| !item.Get()->TryGetValue(
					MakeBindingSourcePropertyToken(L"Region"), regionValue)
				|| !item.Get()->TryGetValue(
					MakeBindingSourcePropertyToken(L"Amount"), amountValue)
				|| !amountValue.TryGetInt64(amount))
				return fail(L"DataGrid 金额排序后的分组项不可读取。");
			const std::wstring region = regionValue.ToString();
			if (index > 0 && region == previousRegion)
			{
				sawInGroupPair = true;
				if (amount < previousAmount)
					return fail(
						L"DataGrid 金额列未在区域组内按 Int64 数值升序："
						+ previousRegion + L"="
						+ std::to_wstring(previousAmount) + L"→"
						+ std::to_wstring(amount) + L"。");
			}
			previousRegion = region;
			previousAmount = amount;
		}
		if (!sawInGroupPair)
			return fail(L"DataGrid 金额排序验证没有覆盖任何区域组内相邻项。");
		auto* firstRowHeader = firstRow ? firstRow->GetRowHeader() : nullptr;
		auto* selectAllButton = dataGrid->GetSelectAllButton();
		if (!firstRowHeader || !selectAllButton
			|| firstRowHeader->GetVisibility() != Visibility::Visible
			|| selectAllButton->GetVisibility() != Visibility::Visible)
			return fail(L"DataGrid All 表头模式未生成可见行表头与全选角。");
		const auto rowHeaderPixels = ContentDipRectToClientPixels(
			firstRowHeader->GetAbsoluteBoundsDip());
		const LPARAM rowHeaderPoint = MAKELPARAM(
			(rowHeaderPixels.left + rowHeaderPixels.right) / 2,
			(rowHeaderPixels.top + rowHeaderPixels.bottom) / 2);
		(void)::SendMessageW(
			Handle, WM_LBUTTONDOWN, MK_LBUTTON, rowHeaderPoint);
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0, rowHeaderPoint);
		if (dataGrid->GetSelectedIndex() != 0
			|| dataGrid->GetSelectedCells().size() != dataGrid->ColumnCount())
		{
			auto* rowHit = cui::framework::WindowAccess::HitTestControlAt(
				*this,
				static_cast<int>((firstRowHeader->GetAbsoluteBoundsDip().left
					+ firstRowHeader->GetAbsoluteBoundsDip().right) * 0.5f),
				static_cast<int>((firstRowHeader->GetAbsoluteBoundsDip().top
					+ firstRowHeader->GetAbsoluteBoundsDip().bottom) * 0.5f));
			bool hitBelongsToRowHeader = false;
			for (auto* current = rowHit; current;
				current = current->GetVisualParent())
				if (current == firstRowHeader)
				{
					hitBelongsToRowHeader = true;
					break;
				}
			const auto bounds = firstRowHeader->GetAbsoluteBoundsDip();
			return fail(
				L"DataGrid 行表头真实点击未在 CellOrRowHeader 模式选择整行："
				L"selected=" + std::to_wstring(dataGrid->GetSelectedIndex())
				+ L"，cells=" + std::to_wstring(
					dataGrid->GetSelectedCells().size())
				+ L"，columns=" + std::to_wstring(dataGrid->ColumnCount())
				+ L"，hit=" + std::to_wstring(
					rowHit ? static_cast<int>(rowHit->Type()) : -1)
				+ L"/" + std::to_wstring(hitBelongsToRowHeader)
				+ L"，bounds=" + std::to_wstring(bounds.left) + L","
				+ std::to_wstring(bounds.top) + L","
				+ std::to_wstring(bounds.right) + L","
				+ std::to_wstring(bounds.bottom)
				+ L"，vOffset=" + std::to_wstring(
					dataScroll ? dataScroll->VerticalOffset : -1.0) + L"。");
		}
		const auto selectAllPixels = ContentDipRectToClientPixels(
			selectAllButton->GetAbsoluteBoundsDip());
		const LPARAM selectAllPoint = MAKELPARAM(
			(selectAllPixels.left + selectAllPixels.right) / 2,
			(selectAllPixels.top + selectAllPixels.bottom) / 2);
		(void)::SendMessageW(
			Handle, WM_LBUTTONDOWN, MK_LBUTTON, selectAllPoint);
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0, selectAllPoint);
		if (dataGrid->GetSelectedIndices().size() != dataGrid->ItemCount()
			|| dataGrid->GetSelectedCells().size()
				!= dataGrid->ItemCount() * dataGrid->ColumnCount())
			return fail(L"DataGrid 左上角真实点击未选择全部行与单元格。");
		// SelectAll owns a stable snapshot of the 18-row projection. It is useful
		// for the interaction assertion above, but must not participate in the
		// million-source replacement or retain the old projection during its gate.
		dataGrid->UnselectAllCells();
		if (!dataGrid->GetSelectedIndices().empty()
			|| !dataGrid->GetSelectedCells().empty())
			return fail(L"DataGrid 百万行 gate 前无法清理 SelectAll 状态。");

		// The scale gate must combine the real HWND input path with the actual
		// million-row lazy source. Structural virtualization alone cannot detect a
		// Win32 paint queue that advances only after wheel/capture input stops.
		auto* millionButton = dynamic_cast<Button*>(
			FindGeneratedControlByName(L"dataGridMillionButton"));
		if (!millionButton || !millionButton->Invoke()
			|| dataGrid->ItemCount() != MillionOrderList::RowCount)
			return fail(L"DataGrid 百万行真实输入 gate 无法安装按需数据源。");
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid 百万行真实输入 gate 无法完成首帧。");
		dataScroll = dynamic_cast<ScrollViewer*>(
			dataGrid->FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_ScrollViewer")));
		headerPresenter = dataGrid->GetColumnHeadersPresenter();
		firstHeader = headerPresenter ? headerPresenter->GetHeader(0) : nullptr;
		if (!dataScroll || !firstHeader || dataGrid->GeneratedItemCount() == 0
			|| dataGrid->GeneratedItemCount() >= 128
			|| dataScroll->ExtentHeight <= dataScroll->ViewportHeight)
			return fail(L"DataGrid 百万行真实输入 gate 未保持稀疏可滚动视口。");

		// Million-row column resize: commit a coalesced frame while native capture
		// is still active. The specialized row presenter must retain row/cell
		// identity and consume the resolved column prefix directly.
		VisibleWindowScope millionDataGridVisibility(Handle);
		if (!drainPresentationWork())
			return fail(L"DataGrid 百万行列宽 gate 无法排空旧帧。");
		clearPresentationTurns();
		const auto millionHeaderPixels = ContentDipRectToClientPixels(
			firstHeader->GetAbsoluteBoundsDip());
		const int millionResizeX = (std::max)(
			millionHeaderPixels.left, millionHeaderPixels.right - 2);
		const int millionResizeY =
			(millionHeaderPixels.top + millionHeaderPixels.bottom) / 2;
		const LPARAM millionResizeStart = MAKELPARAM(
			millionResizeX, millionResizeY);
		auto* millionColumn = firstHeader->GetColumn();
		const double millionWidthBefore = millionColumn
			? millionColumn->GetWidth().Value : -1.0;
		auto* millionHeaderIdentity = firstHeader;
		auto* millionRowIdentity = dynamic_cast<DataGridRow*>(
			dataGrid->GetGeneratedItem(0));
		(void)::SendMessageW(Handle, WM_MOUSEMOVE, 0, millionResizeStart);
		(void)::SendMessageW(
			Handle, WM_LBUTTONDOWN, MK_LBUTTON, millionResizeStart);
		clearPresentationTurns();
		const auto committedBeforeMillionResize =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		for (int step = 1; step <= 16; ++step)
			(void)::SendMessageW(Handle, WM_MOUSEMOVE, MK_LBUTTON,
				MAKELPARAM(millionResizeX + step, millionResizeY));
		const bool millionResizeTurn = dispatchPresentationTurn();
		const bool millionResizeFrameDrained = drainPresentationWork();
		const auto committedDuringMillionResize =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		const bool millionResizeCaptured =
			GetMouseCaptured() == millionHeaderIdentity;
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0,
			MAKELPARAM(millionResizeX + 16, millionResizeY));
		firstHeader = headerPresenter ? headerPresenter->GetHeader(0) : nullptr;
		millionColumn = firstHeader ? firstHeader->GetColumn() : nullptr;
		const double millionExpectedResizeDelta = 16.0
			/ static_cast<double>((std::max)(0.001f, GetDpiScale()));
		if (!millionResizeFrameDrained || !millionResizeCaptured
			|| committedDuringMillionResize <= committedBeforeMillionResize
			|| firstHeader != millionHeaderIdentity
			|| dataGrid->GetGeneratedItem(0) != millionRowIdentity
			|| !millionColumn
			|| std::abs(millionColumn->GetWidth().Value
				- (millionWidthBefore + millionExpectedResizeDelta)) > 3.0)
			return fail(L"DataGrid 百万行列宽拖动未在 PointerUp 前提交可见帧："
				L"dispatch=" + std::to_wstring(millionResizeTurn)
				+ L"，drained=" + std::to_wstring(millionResizeFrameDrained)
				+ L"，capture=" + std::to_wstring(millionResizeCaptured)
				+ L"，frame="
				+ std::to_wstring(committedBeforeMillionResize) + L"→"
				+ std::to_wstring(committedDuringMillionResize) + L"。");

		// Wheel burst: commit after the first half, then continue sending wheel
		// input. This proves the frame is not an end-of-stream side effect.
		dataScroll->ScrollToVerticalOffset(0.0);
		if (!drainPresentationWork())
			return fail(L"DataGrid 百万行滚轮 gate 无法回到顶部。");
		clearPresentationTurns();
		const auto dataGridPixels = ContentDipRectToClientPixels(
			dataGrid->GetAbsoluteBoundsDip());
		POINT wheelClient{
			(dataGridPixels.left + dataGridPixels.right) / 2,
			(dataGridPixels.top + dataGridPixels.bottom) / 2 };
		POINT wheelScreen = wheelClient;
		(void)::ClientToScreen(Handle, &wheelScreen);
		const LPARAM wheelPoint = MAKELPARAM(wheelScreen.x, wheelScreen.y);
		const auto committedBeforeWheel =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		for (int step = 0; step < 8; ++step)
			(void)::SendMessageW(Handle, WM_MOUSEWHEEL,
				MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), wheelPoint);
		const bool wheelTurn = dispatchPresentationTurn();
		const bool wheelFrameDrained = drainPresentationWork();
		const auto committedDuringWheel =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		const double wheelOffsetDuringBurst = dataScroll->VerticalOffset;
		for (int step = 0; step < 8; ++step)
			(void)::SendMessageW(Handle, WM_MOUSEWHEEL,
				MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), wheelPoint);
		if (!wheelFrameDrained
			|| committedDuringWheel <= committedBeforeWheel
			|| wheelOffsetDuringBurst <= 0.0
			|| dataGrid->GetGeneratedItem(0) != nullptr
			|| dataGrid->GeneratedItemCount() == 0
			|| dataGrid->GeneratedItemCount() >= 128)
			return fail(L"DataGrid 百万行快速滚轮未在输入流中间提交可见帧："
				L"dispatch=" + std::to_wstring(wheelTurn)
				+ L"，drained=" + std::to_wstring(wheelFrameDrained)
				+ L"，offset=" + std::to_wstring(wheelOffsetDuringBurst)
				+ L"，frame=" + std::to_wstring(committedBeforeWheel)
				+ L"→" + std::to_wstring(committedDuringWheel) + L"。");

		// Direct Thumb burst: move near the tail, pump exactly the framework
		// presentation turn while capture remains held, then release.
		if (!drainPresentationWork())
			return fail(L"DataGrid 百万行 Thumb gate 无法排空滚轮帧。");
		clearPresentationTurns();
		const auto dataScrollSize = dataScroll->GetActualSizeDip();
		const auto dataScrollBounds = dataScroll->GetAbsoluteBoundsDip();
		const float millionThumbHeight = std::clamp(
			static_cast<float>(dataScroll->ViewportHeight
				* dataScroll->ViewportHeight / dataScroll->ExtentHeight),
			std::max(16.0f,
				static_cast<float>(dataScroll->ViewportHeight * 0.1)),
			static_cast<float>(dataScroll->ViewportHeight));
		const int millionBarX = static_cast<int>(std::floor(
			(dataScroll->ViewportWidth + dataScrollSize.width) * 0.5));
		const int millionThumbStartY = static_cast<int>(
			std::floor(millionThumbHeight * 0.5f));
		const int millionThumbEndY = static_cast<int>(std::floor(
			dataScroll->ViewportHeight - millionThumbHeight * 0.5f));
		auto dataScrollClientPoint = [&](int localX, int localY)
		{
			const auto pixel = ContentDipRectToClientPixels(D2D1::RectF(
				dataScrollBounds.left + static_cast<float>(localX),
				dataScrollBounds.top + static_cast<float>(localY),
				dataScrollBounds.left + static_cast<float>(localX) + 1.0f,
				dataScrollBounds.top + static_cast<float>(localY) + 1.0f));
			return MAKELPARAM(pixel.left, pixel.top);
		};
		(void)::SendMessageW(Handle, WM_LBUTTONDOWN, MK_LBUTTON,
			dataScrollClientPoint(millionBarX, millionThumbStartY));
		clearPresentationTurns();
		const auto committedBeforeMillionThumb =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		for (int step = 1; step <= 24; ++step)
		{
			const int localY = millionThumbStartY
				+ (millionThumbEndY - millionThumbStartY) * step / 24;
			(void)::SendMessageW(Handle, WM_MOUSEMOVE, MK_LBUTTON,
				dataScrollClientPoint(millionBarX, localY));
		}
		const bool millionThumbTurn = dispatchPresentationTurn();
		// Offset invalidation is synchronous, but the private dispatch token may
		// only enqueue the ordinary paint transaction (or may already have been
		// consumed by a nested native dispatch). Drain retained paint work without
		// pumping arbitrary input while Thumb capture is still active.
		const bool millionThumbFrameDrained = drainPresentationWork();
		const auto committedDuringMillionThumb =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		const bool millionThumbCaptured = GetMouseCaptured() == dataScroll;
		const double millionThumbOffset = dataScroll->VerticalOffset;
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0,
			dataScrollClientPoint(millionBarX, millionThumbEndY));
		if (!millionThumbFrameDrained || !millionThumbCaptured
			|| GetMouseCaptured() != nullptr
			|| committedDuringMillionThumb <= committedBeforeMillionThumb
			|| millionThumbOffset <= (dataScroll->ExtentHeight
				- dataScroll->ViewportHeight) * 0.5
			|| dataGrid->GetGeneratedItem(0) != nullptr
			|| dataGrid->GeneratedItemCount() == 0
			|| dataGrid->GeneratedItemCount() >= 128)
			return fail(L"DataGrid 百万行 Thumb 拖动未在 PointerUp 前提交尾部帧："
				L"dispatch=" + std::to_wstring(millionThumbTurn)
				+ L"，drained=" + std::to_wstring(millionThumbFrameDrained)
				+ L"，capture=" + std::to_wstring(millionThumbCaptured)
				+ L"，offset=" + std::to_wstring(millionThumbOffset)
				+ L"，frame="
				+ std::to_wstring(committedBeforeMillionThumb) + L"→"
				+ std::to_wstring(committedDuringMillionThumb) + L"。");
		millionDataGridVisibility.Restore();

		if (!millionButton->Invoke())
			return fail(L"DataGrid 百万行真实输入 gate 无法恢复示例数据。");
		const size_t restoredProjectionCount =
			18 + (dataGrid->GetCanUserAddRows() ? 1 : 0);
		if (!dataGrid->GetItemsSource()
			|| dataGrid->GetItemsSource().Get()->Count() != 18
			|| dataGrid->ItemCount() != restoredProjectionCount)
			return fail(L"DataGrid 百万行真实输入 gate 无法恢复示例数据。");
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"DataGrid 百万行真实输入 gate 恢复后未稳定绘制。");

		// A native animation tick is a retained content invalidation. It must
		// schedule one local future frame and never synchronously re-enter paint
		// while a pointer/caption/modal transaction is still unwinding.
		_tabs->SelectedIndex = containerTabIndex;
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		const bool loadingRingPresentationDrained =
			drainPresentationWork();
		if (!loadingRingPresentationDrained
			|| !loadingRing->IsVisible || !loadingRing->IsAnimationRunning())
			return fail(L"LoadingRing smoke 未进入稳定可见 retained 场景："
				L"drained=" + std::to_wstring(loadingRingPresentationDrained)
				+ L"，visible=" + std::to_wstring(loadingRing->IsVisible)
				+ L"，running="
				+ std::to_wstring(loadingRing->IsAnimationRunning())
				+ L"，selected="
				+ std::to_wstring(_tabs->SelectedIndex)
				+ L"，target=" + std::to_wstring(containerTabIndex)
				+ L"。");
		const auto animationSceneRevision =
			cui::framework::WindowAccess::PresentationSceneRevision(*this);
		const auto animationGeometryRevision =
			cui::framework::WindowAccess::PresentationGeometryRevision(*this);
		const auto animationContentRevision =
			cui::framework::WindowAccess::PresentationContentRevision(*this);
		const auto committedBeforeAnimation =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		if (!cui::framework::PresentationAccess::
			InvalidateNativeAnimationFrame(*loadingRing))
			return fail(L"LoadingRing smoke 无法排队原生动画叶节点。");
		RECT animationOsDamage{};
		const bool animationOsDamageQueued =
			::GetUpdateRect(Handle, &animationOsDamage, FALSE) != FALSE;
		const bool animationFrameworkDamageQueued =
			cui::framework::WindowAccess::HasPendingRenderWork(*this);
		const bool animationRetainedDamageQueued =
			cui::framework::WindowAccess::
				HasPendingPresentationDamage(*this);
		const auto committedAfterAnimationTick =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		const bool visibleWindowMissingOsDamage =
			::IsWindowVisible(Handle) != FALSE && !animationOsDamageQueued;
		if (!animationRetainedDamageQueued
			|| !animationFrameworkDamageQueued
			|| visibleWindowMissingOsDamage
			|| committedAfterAnimationTick != committedBeforeAnimation)
		{
			const auto animationBounds = loadingRing->GetAbsoluteBoundsDip();
			return fail(L"LoadingRing tick 未异步排队局部 damage，或同步重入了绘制："
				L"os=" + std::to_wstring(animationOsDamageQueued)
				+ L"，framework="
				+ std::to_wstring(animationFrameworkDamageQueued)
				+ L"，retained="
				+ std::to_wstring(animationRetainedDamageQueued)
				+ L"，visible="
				+ std::to_wstring(::IsWindowVisible(Handle) != FALSE)
				+ L"，committed="
				+ std::to_wstring(committedBeforeAnimation) + L"→"
				+ std::to_wstring(committedAfterAnimationTick)
				+ L"，damage=("
				+ std::to_wstring(animationOsDamage.left) + L","
				+ std::to_wstring(animationOsDamage.top) + L","
				+ std::to_wstring(animationOsDamage.right) + L","
				+ std::to_wstring(animationOsDamage.bottom) + L")"
				+ L"，bounds=("
				+ std::to_wstring(animationBounds.left) + L","
				+ std::to_wstring(animationBounds.top) + L","
				+ std::to_wstring(animationBounds.right) + L","
				+ std::to_wstring(animationBounds.bottom) + L")。");
		}
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		RECT animationDirty{};
		bool animationWasFull = true;
		const auto animationFrame =
			cui::framework::WindowAccess::PresentationFrame(*this);
		PresentationNodeSnapshot animationNode{};
		const bool hasAnimationNode =
			cui::framework::WindowAccess::
				TryGetPresentationNodeSnapshot(
					*this, loadingRing, animationNode);
		const size_t animationSubmittedNodes =
			animationFrame.CommandRecordedNodes
			+ animationFrame.ImmediateDrawNodes
			+ animationFrame.NativeCommitNodes;
		if (!cui::framework::WindowAccess::TryGetLastRenderDirtyRect(
				*this, animationDirty, animationWasFull)
			|| animationWasFull
			|| cui::framework::WindowAccess::
				PresentationSceneRevision(*this) != animationSceneRevision
			|| cui::framework::WindowAccess::
				PresentationGeometryRevision(*this)
				!= animationGeometryRevision
			|| cui::framework::WindowAccess::
				PresentationContentRevision(*this)
				<= animationContentRevision
			|| cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this)
				<= committedBeforeAnimation
			|| animationFrame.ContentDirtyNodes == 0
			|| animationSubmittedNodes == 0
			|| !hasAnimationNode || animationNode.ContentDirty
			|| !animationNode.HasPresented)
		{
			return fail(L"LoadingRing tick 未保持局部 retained content-only 帧："
				L"full=" + std::to_wstring(animationWasFull)
				+ L"，dirty=("
				+ std::to_wstring(animationDirty.left) + L","
				+ std::to_wstring(animationDirty.top) + L","
				+ std::to_wstring(animationDirty.right) + L","
				+ std::to_wstring(animationDirty.bottom) + L")"
				+ L"，scene="
				+ std::to_wstring(animationSceneRevision) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationSceneRevision(*this))
				+ L"，geometry="
				+ std::to_wstring(animationGeometryRevision) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationGeometryRevision(*this))
				+ L"，content="
				+ std::to_wstring(animationContentRevision) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationContentRevision(*this))
				+ L"，committed="
				+ std::to_wstring(committedBeforeAnimation) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationCommittedFrameCount(*this))
				+ L"，contentDirty="
				+ std::to_wstring(animationFrame.ContentDirtyNodes)
				+ L"，recorded="
				+ std::to_wstring(animationFrame.CommandRecordedNodes)
				+ L"，replayed="
				+ std::to_wstring(animationFrame.CommandReplayedNodes)
				+ L"，culled="
				+ std::to_wstring(animationFrame.CulledNodes)
				+ L"，immediate="
				+ std::to_wstring(animationFrame.ImmediateDrawNodes)
				+ L"，native="
				+ std::to_wstring(animationFrame.NativeCommitNodes)
				+ L"，node="
				+ std::to_wstring(hasAnimationNode)
				+ L"/dirty="
				+ std::to_wstring(animationNode.ContentDirty)
				+ L"/commands="
				+ std::to_wstring(animationNode.HasDrawingCommands)
				+ L"/presented="
				+ std::to_wstring(animationNode.HasPresented)
				+ L"/native="
				+ std::to_wstring(animationNode.NativeComposition)
				+ L"/overlay="
				+ std::to_wstring(animationNode.Overlay)
				+ L"/bounds=("
				+ std::to_wstring(animationNode.RenderedBounds.left) + L","
				+ std::to_wstring(animationNode.RenderedBounds.top) + L","
				+ std::to_wstring(animationNode.RenderedBounds.right) + L","
				+ std::to_wstring(animationNode.RenderedBounds.bottom) + L")"
				+ L"。");
		}

		// Exercise the real scrollbar gesture path, then prove that the
		// ancestor scroll transform invalidates every retained descendant
		// geometry/command transform before the next replay.
		_tabs->SelectedIndex = layoutTabIndex;
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork() || !scroll->IsVisible
			|| scroll->ExtentWidth <= scroll->ViewportWidth
			|| scroll->ExtentHeight <= scroll->ViewportHeight)
			return fail(L"ScrollViewer smoke 未形成双轴可滚动 viewport。");
		VisibleWindowScope scrollThumbVisibility(Handle);
		scroll->ScrollToHome();
		if (!drainPresentationWork())
			return fail(L"ScrollViewer smoke 无法排空初始滚动 damage。");
		clearPresentationTurns();
		const auto scrollSize = scroll->GetActualSizeDip();
		const float verticalThumbHeight = std::clamp(
			static_cast<float>(scroll->ViewportHeight * scroll->ViewportHeight
				/ scroll->ExtentHeight),
			std::max(16.0f,
				static_cast<float>(scroll->ViewportHeight * 0.1)),
			static_cast<float>(scroll->ViewportHeight));
		const int verticalBarX = static_cast<int>(std::floor(
			(scroll->ViewportWidth + scrollSize.width) * 0.5));
		const int verticalStartY = static_cast<int>(
			std::floor(verticalThumbHeight * 0.5f));
		const int verticalEndY = static_cast<int>(std::floor(
			scroll->ViewportHeight - verticalThumbHeight * 0.5f));
		const auto scrollBounds = scroll->GetAbsoluteBoundsDip();
		auto scrollClientPoint = [&](int localX, int localY)
		{
			const auto pixel = ContentDipRectToClientPixels(D2D1::RectF(
				scrollBounds.left + static_cast<float>(localX),
				scrollBounds.top + static_cast<float>(localY),
				scrollBounds.left + static_cast<float>(localX) + 1.0f,
				scrollBounds.top + static_cast<float>(localY) + 1.0f));
			return MAKELPARAM(pixel.left, pixel.top);
		};
		const LPARAM verticalStart =
			scrollClientPoint(verticalBarX, verticalStartY);
		(void)::SendMessageW(
			Handle, WM_LBUTTONDOWN, MK_LBUTTON, verticalStart);
		const bool verticalThumbCaptured = GetMouseCaptured() == scroll;
		// Isolate the move burst from focus/press invalidation, while proving the
		// framework can commit a turn without ending the active Thumb capture.
		clearPresentationTurns();
		const bool verticalCaptureSurvivedPressPresentation =
			GetMouseCaptured() == scroll;
		const auto scrollProbeBefore = scrollProbe->GetAbsoluteLocationDip();
		const auto scrollSceneRevision =
			cui::framework::WindowAccess::PresentationSceneRevision(*this);
		const auto scrollGeometryRevision =
			cui::framework::WindowAccess::PresentationGeometryRevision(*this);
		const auto committedBeforeScroll =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		for (int step = 1; step <= 24; ++step)
		{
			const int localY = verticalStartY
				+ (verticalEndY - verticalStartY) * step / 24;
			(void)::SendMessageW(
				Handle, WM_MOUSEMOVE, MK_LBUTTON,
				scrollClientPoint(verticalBarX, localY));
		}
		const bool verticalPresentationTurnDispatched =
			dispatchPresentationTurn();
		const auto committedDuringVerticalThumb =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		const bool verticalCaptureSurvivedPresentation =
			GetMouseCaptured() == scroll;
		(void)::SendMessageW(
			Handle, WM_LBUTTONUP, 0,
			scrollClientPoint(verticalBarX, verticalEndY));
		const bool verticalThumbReleased = GetMouseCaptured() == nullptr;

		const float horizontalThumbWidth = std::clamp(
			static_cast<float>(scroll->ViewportWidth * scroll->ViewportWidth
				/ scroll->ExtentWidth),
			std::max(16.0f,
				static_cast<float>(scroll->ViewportWidth * 0.1)),
			static_cast<float>(scroll->ViewportWidth));
		const int horizontalBarY = static_cast<int>(std::floor(
			(scroll->ViewportHeight + scrollSize.height) * 0.5));
		const int horizontalStartX = static_cast<int>(
			std::floor(horizontalThumbWidth * 0.5f));
		const int horizontalEndX = static_cast<int>(std::floor(
			scroll->ViewportWidth - horizontalThumbWidth * 0.5f));
		(void)cui::framework::InputAccess::DispatchInput(
			*scroll, PointerInput(InputReportKind::PointerDown,
				MouseButton::Left, horizontalStartX, horizontalBarY,
				MouseButton::Left));
		(void)cui::framework::InputAccess::DispatchInput(
			*scroll, PointerInput(InputReportKind::PointerMove,
				MouseButton::None, horizontalEndX, horizontalBarY,
				MouseButton::Left));
		(void)cui::framework::InputAccess::DispatchInput(
			*scroll, PointerInput(InputReportKind::PointerUp,
				MouseButton::Left, horizontalEndX, horizontalBarY));
		const auto scrollProbeAfter = scrollProbe->GetAbsoluteLocationDip();
		RECT scrollOsDamage{};
		const bool scrollOsDamageQueued =
			::GetUpdateRect(Handle, &scrollOsDamage, FALSE) != FALSE;
		const bool scrollFrameworkDamageQueued =
			cui::framework::WindowAccess::HasPendingRenderWork(*this);
		const bool scrollRetainedDamageQueued =
			cui::framework::WindowAccess::
				HasPendingPresentationDamage(*this);
		const auto committedAfterScrollGesture =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		const bool visibleScrollWindowMissingOsDamage =
			::IsWindowVisible(Handle) != FALSE && !scrollOsDamageQueued;
		if (scroll->HorizontalOffset <= 0.0 || scroll->VerticalOffset <= 0.0
			|| std::fabs(
				(scrollProbeBefore.x - scrollProbeAfter.x)
					- scroll->HorizontalOffset) > 1.0
			|| std::fabs(
				(scrollProbeBefore.y - scrollProbeAfter.y)
					- scroll->VerticalOffset) > 1.0
			|| !scrollRetainedDamageQueued
			|| !scrollFrameworkDamageQueued
			|| visibleScrollWindowMissingOsDamage
			|| !verticalThumbCaptured
			|| !verticalCaptureSurvivedPressPresentation
			|| !verticalPresentationTurnDispatched
			|| !verticalCaptureSurvivedPresentation
			|| !verticalThumbReleased
			|| committedDuringVerticalThumb <= committedBeforeScroll
			|| committedAfterScrollGesture != committedDuringVerticalThumb)
			return fail(L"ScrollViewer 滚动条手势未更新双轴 offset/后代坐标或未排队绘制："
				L"offset=("
				+ std::to_wstring(scroll->HorizontalOffset) + L","
				+ std::to_wstring(scroll->VerticalOffset) + L")"
				+ L"，probe=("
				+ std::to_wstring(scrollProbeBefore.x) + L","
				+ std::to_wstring(scrollProbeBefore.y) + L")→("
				+ std::to_wstring(scrollProbeAfter.x) + L","
				+ std::to_wstring(scrollProbeAfter.y) + L")"
				+ L"，os=" + std::to_wstring(scrollOsDamageQueued)
				+ L"，framework="
				+ std::to_wstring(scrollFrameworkDamageQueued)
				+ L"，retained="
				+ std::to_wstring(scrollRetainedDamageQueued)
				+ L"，visible="
				+ std::to_wstring(::IsWindowVisible(Handle) != FALSE)
				+ L"，capture=" + std::to_wstring(verticalThumbCaptured)
				+ L"/"
				+ std::to_wstring(verticalCaptureSurvivedPressPresentation)
				+ L"/"
				+ std::to_wstring(verticalCaptureSurvivedPresentation)
				+ L"/" + std::to_wstring(verticalThumbReleased)
				+ L"，dispatch="
				+ std::to_wstring(verticalPresentationTurnDispatched)
				+ L"，committed="
				+ std::to_wstring(committedBeforeScroll) + L"→"
				+ std::to_wstring(committedDuringVerticalThumb) + L"→"
				+ std::to_wstring(committedAfterScrollGesture)
				+ L"。");
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		const auto scrollFrame =
			cui::framework::WindowAccess::PresentationFrame(*this);
		if (cui::framework::WindowAccess::
				PresentationSceneRevision(*this) != scrollSceneRevision
			|| cui::framework::WindowAccess::
				PresentationGeometryRevision(*this)
				<= scrollGeometryRevision
			|| cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this)
				<= committedBeforeScroll
			|| scrollFrame.GeometryDirtyNodes < 2
			|| scrollFrame.GeometryRecomputedNodes < 2
			|| scrollFrame.CommandRecordedNodes
				+ scrollFrame.ImmediateDrawNodes < 2)
			return fail(L"ScrollViewer offset 未使 retained 后代几何与命令变换重录："
				L"scene="
				+ std::to_wstring(scrollSceneRevision) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationSceneRevision(*this))
				+ L"，geometry="
				+ std::to_wstring(scrollGeometryRevision) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationGeometryRevision(*this))
				+ L"，committed="
				+ std::to_wstring(committedBeforeScroll) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationCommittedFrameCount(*this))
				+ L"，dirty="
				+ std::to_wstring(scrollFrame.GeometryDirtyNodes)
				+ L"，recomputed="
				+ std::to_wstring(scrollFrame.GeometryRecomputedNodes)
				+ L"，recorded="
				+ std::to_wstring(scrollFrame.CommandRecordedNodes)
				+ L"，immediate="
				+ std::to_wstring(scrollFrame.ImmediateDrawNodes)
				+ L"。");
		scrollThumbVisibility.Restore();

		// Entering the browser page replaces the HWND swap chain with the
		// DirectComposition surface tree. Every newly created scene swap chain
		// must submit one complete history-establishing frame before Present1
		// dirty rectangles are legal. Leaving the page must remain renderable.
		if (!_web->TrySetHtml(
			L"<!doctype html><meta charset='utf-8'><button>transform smoke</button>"))
			return fail(L"WebBrowser render smoke 无法排队离线 HTML 内容。");
		const auto abortedBeforeBrowser =
			cui::framework::WindowAccess::
				PresentationAbortedFrameCount(*this);
		const auto committedBeforeBrowser =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		_tabs->SelectedIndex = browserTabIndex;
		RequestLayout();
		UpdateLayout();
		const auto browserTransform = _web->GetRenderTransform();
		const auto browserSize = _web->GetActualSizeDip();
		auto* browserViewport = _web->GetVisualParent();
		const bool browserViewportPaintsBackdrop = browserViewport
			&& browserViewport->Background.Kind
				!= cui::drawing::BrushKind::None;
		const auto browserMatrix = _web->GetLocalToRenderTransform();
		const auto browserCenter = D2D1::Matrix3x2F(
			browserMatrix._11, browserMatrix._12,
			browserMatrix._21, browserMatrix._22,
			browserMatrix._31, browserMatrix._32)
			.TransformPoint(D2D1::Point2F(
				browserSize.width * 0.5f, browserSize.height * 0.5f));
		auto* browserHit = cui::framework::WindowAccess::HitTestControlAt(
			*this, static_cast<int>(std::lround(browserCenter.x)),
			static_cast<int>(std::lround(browserCenter.y)));
		if (!browserTransform || browserTransform->Operations.size() != 2
			|| browserSize.width <= 0.0f || browserSize.height <= 0.0f
			|| _web->DefaultBackgroundColor.a > 1e-6f
			|| std::fabs(_web->CornerRadius.TopLeft - 6.0f) > 1e-6f
			|| std::fabs(_web->CornerRadius.TopRight - 6.0f) > 1e-6f
			|| std::fabs(_web->CornerRadius.BottomRight - 6.0f) > 1e-6f
			|| std::fabs(_web->CornerRadius.BottomLeft - 6.0f) > 1e-6f
			|| !browserViewport || browserViewport->ClipsChildren()
			|| browserViewportPaintsBackdrop
			|| browserHit != _web)
			return fail(L"WebBrowser 的声明式变换未进入实际布局/命中链："
				L"operations=" + std::to_wstring(
					browserTransform ? browserTransform->Operations.size() : 0)
				+ L"，size=(" + std::to_wstring(browserSize.width) + L","
				+ std::to_wstring(browserSize.height) + L")，center=("
				+ std::to_wstring(browserCenter.x) + L","
				+ std::to_wstring(browserCenter.y) + L")，overflow="
				+ std::to_wstring(browserViewport != nullptr) + L"/"
				+ std::to_wstring(browserViewport
					&& !browserViewport->ClipsChildren()) + L"，backdrop="
				+ std::to_wstring(browserViewportPaintsBackdrop) + L"。");
		Invalidate(false);
		if (!drainPresentationWork()
			|| !_web->IsVisible
			|| !GetDCompDevice()
			|| cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this)
				<= committedBeforeBrowser
			|| cui::framework::WindowAccess::
				PresentationAbortedFrameCount(*this)
				!= abortedBeforeBrowser)
			return fail(L"WebBrowser 页未建立可提交的 DirectComposition 首帧："
				L"committed="
				+ std::to_wstring(committedBeforeBrowser) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationCommittedFrameCount(*this))
				+ L"，aborted="
				+ std::to_wstring(abortedBeforeBrowser) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationAbortedFrameCount(*this))
				+ L"，surfaceFailure="
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationLastSurfaceFailureSequence(*this))
				+ L"/" + std::to_wstring(cui::framework::WindowAccess::
					PresentationLastFailedSurfaceRole(*this))
				+ L"，present=0x" + StringHelper::Format(
					L"%08X", static_cast<unsigned int>(
						cui::framework::WindowAccess::
							PresentationLastFailedPresentHr(*this)))
				+ L"。");

		const ULONGLONG webViewDeadline = ::GetTickCount64() + 8000;
		while (!_web->IsWebViewReady()
			&& _web->GetInitializationState()
				!= WebBrowser::InitializationState::Failed
			&& _web->GetInitializationState()
				!= WebBrowser::InitializationState::Unsupported
			&& ::GetTickCount64() < webViewDeadline)
		{
			MSG message{};
			while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
			{
				if (message.message == WM_QUIT) break;
				::TranslateMessage(&message);
				::DispatchMessageW(&message);
			}
			(void)drainPresentationWork();
			if (!_web->IsWebViewReady())
				(void)::MsgWaitForMultipleObjectsEx(
					0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		}
		if (!_web->IsWebViewReady()
			|| FAILED(_web->GetLastWebViewError()))
			return fail(L"WebView2 composition controller 未在 render smoke 中就绪："
				L"state=" + std::to_wstring(static_cast<int>(
					_web->GetInitializationState()))
				+ L"，environment=0x" + StringHelper::Format(
					L"%08X", static_cast<unsigned int>(
						_web->GetLastEnvironmentError()))
				+ L"，controller=0x" + StringHelper::Format(
					L"%08X", static_cast<unsigned int>(
						_web->GetLastControllerError())) + L"。");

		const auto browserClientPoint = ContentDipRectToClientPixels(
			D2D1::RectF(browserCenter.x, browserCenter.y,
				browserCenter.x + 1.0f, browserCenter.y + 1.0f));
		const LPARAM browserPointer = MAKELPARAM(
			browserClientPoint.left, browserClientPoint.top);
		(void)::SendMessageW(
			Handle, WM_LBUTTONDOWN, MK_LBUTTON, browserPointer);
		if (GetMouseCaptured() != _web)
			return fail(L"变换后的 WebView2 未接受实际鼠标按下或建立捕获。");
		(void)::SendMessageW(Handle, WM_LBUTTONUP, 0, browserPointer);
		if (GetMouseCaptured())
			return fail(L"WebView2 鼠标抬起后未释放 CUI 捕获。");

		const auto committedOnBrowser =
			cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this);
		_tabs->SelectedIndex = layoutTabIndex;
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork()
			|| !scroll->IsVisible
			|| _web->IsVisible
			|| cui::framework::WindowAccess::
				PresentationCommittedFrameCount(*this)
				<= committedOnBrowser
			|| cui::framework::WindowAccess::
				PresentationAbortedFrameCount(*this)
				!= abortedBeforeBrowser)
			return fail(L"离开 WebBrowser 页后合成宿主未继续刷新普通控件页："
				L"committed="
				+ std::to_wstring(committedOnBrowser) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationCommittedFrameCount(*this))
				+ L"，aborted="
				+ std::to_wstring(abortedBeforeBrowser) + L"→"
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationAbortedFrameCount(*this))
				+ L"。");

		_tabs->SelectedIndex = presentationTabIndex;
		RequestLayout();
		UpdateLayout();
		if (!surface->IsVisible || !topologyTile->IsVisible)
			return fail(L"Presentation smoke 激活页未进入有效可见树。");
		if (GetDrawingContext())
			return fail(L"Presentation smoke 在 frame transaction 外泄漏 DrawingContext。");
		if (!GetDCompDevice())
			return fail(L"Presentation smoke 无法建立 DirectComposition device。");

		const int firstFrame = behavior->FrameCount();
		Invalidate(false);
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		RECT firstRenderDirty{};
		bool firstWasFull = false;
		const bool hasFirstFrame =
			cui::framework::WindowAccess::TryGetLastRenderDirtyRect(
				*this, firstRenderDirty, firstWasFull);
		if (behavior->FrameCount() <= firstFrame
			|| !hasFirstFrame
			|| !firstWasFull)
			return fail(L"Presentation host 未提交首个完整帧：behavior="
				+ std::to_wstring(firstFrame) + L"→"
				+ std::to_wstring(behavior->FrameCount())
				+ L"，last=" + std::to_wstring(hasFirstFrame)
				+ L"/" + std::to_wstring(firstWasFull)
				+ L"，transaction=" + std::to_wstring(
					cui::framework::WindowAccess::
						PresentationTransactionSequence(*this))
				+ L"，committed=" + std::to_wstring(
					cui::framework::WindowAccess::
						PresentationCommittedFrameCount(*this))
				+ L"，aborted=" + std::to_wstring(
					cui::framework::WindowAccess::
						PresentationAbortedFrameCount(*this))
				+ L"，surfaceFailure="
				+ std::to_wstring(cui::framework::WindowAccess::
					PresentationLastSurfaceFailureSequence(*this))
				+ L"/" + std::to_wstring(cui::framework::WindowAccess::
					PresentationLastFailedSurfaceRole(*this))
				+ L"，endDraw=0x" + StringHelper::Format(
					L"%08X", static_cast<unsigned int>(
						cui::framework::WindowAccess::
							PresentationLastFailedEndDrawHr(*this)))
				+ L"，present=0x" + StringHelper::Format(
					L"%08X", static_cast<unsigned int>(
						cui::framework::WindowAccess::
							PresentationLastFailedPresentHr(*this)))
				+ L"，pending=" + std::to_wstring(
					cui::framework::WindowAccess::
						HasPendingRenderWork(*this)) + L"。");

		// The first retained pass may discover additional presentation damage.
		// Drain that work before proving that a later local invalidation remains local.
		for (int pass = 0; pass < 4; ++pass)
		{
			RECT pending{};
			const bool osPending =
				::GetUpdateRect(Handle, &pending, FALSE) != FALSE;
			if (!osPending && !cui::framework::WindowAccess::HasPendingRenderWork(*this)) break;
			(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		}
		RECT pendingAfterFullFrame{};
		if (::GetUpdateRect(Handle, &pendingAfterFullFrame, FALSE)
			|| cui::framework::WindowAccess::HasPendingRenderWork(*this))
			return fail(L"Presentation 完整帧后仍遗留未消费的损伤。");
		const auto stableRevision = cui::framework::WindowAccess::PresentationSceneRevision(*this);
		const auto stableContentRevision = cui::framework::WindowAccess::PresentationContentRevision(*this);
		const auto stableGeometryRevision = cui::framework::WindowAccess::PresentationGeometryRevision(*this);
		const auto stableCompositionRevision =
			cui::framework::WindowAccess::PresentationCompositionRevision(*this);
		const auto stableNodeCount = cui::framework::WindowAccess::PresentationNodeCount(*this);
		const auto stableLayerCount = cui::framework::WindowAccess::PresentationDrawingLayerCount(*this);
		if (stableRevision == 0 || stableNodeCount == 0
			|| stableLayerCount == 0)
			return fail(L"PresentationScene 未生成 retained node/segment 快照。");

		const int regionBefore = behavior->RegionRequests();
		const int frameBefore = behavior->FrameCount();
		behavior->Pulse(*surface);
		RECT pendingDirty{};
		const bool osRegionPending =
			::GetUpdateRect(Handle, &pendingDirty, FALSE) != FALSE;
		const bool regionRequestRecorded =
			behavior->RegionRequests() == regionBefore + 1;
		const bool frameworkDamagePending = cui::framework::WindowAccess::HasPendingRenderWork(*this);
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		const bool regionFrameSubmitted = behavior->FrameCount() > frameBefore;
		RECT regionRenderDirty{};
		bool regionWasFull = true;
		const bool hasRegionFrame = cui::framework::WindowAccess::TryGetLastRenderDirtyRect(*this,
			regionRenderDirty, regionWasFull);
		const long long dirtyArea = static_cast<long long>(
			std::max<LONG>(0, regionRenderDirty.right - regionRenderDirty.left))
			* std::max<LONG>(0, regionRenderDirty.bottom - regionRenderDirty.top);
		const float dpiScale = std::max(0.001f, GetDpiScale());
		const long long clientArea = static_cast<long long>(
			std::ceil((client.right - client.left) / dpiScale))
			* static_cast<long long>(
				std::ceil((client.bottom - client.top) / dpiScale));
		if (!regionRequestRecorded)
			return fail(L"Presentation behavior 未记录局部失效请求。");
		if (!frameworkDamagePending)
			return fail(L"NativeSurface 局部失效未进入 presentation damage queue。");
		if (!hasRegionFrame || regionWasFull
			|| dirtyArea <= 0 || dirtyArea >= clientArea)
			return fail(L"NativeSurface 局部失效被提升为空区域或整窗口：osRegion="
				+ std::to_wstring(osRegionPending) + L", dirty=("
				+ std::to_wstring(regionRenderDirty.left) + L"," + std::to_wstring(regionRenderDirty.top)
				+ L"," + std::to_wstring(regionRenderDirty.right) + L"," + std::to_wstring(regionRenderDirty.bottom)
				+ L"), client=(" + std::to_wstring(client.left) + L"," + std::to_wstring(client.top)
				+ L"," + std::to_wstring(client.right) + L"," + std::to_wstring(client.bottom) + L")。");
		if (!regionFrameSubmitted)
			return fail(L"Presentation scene 未在 region-only 后续帧重绘 NativeSurface。");
		if (cui::framework::WindowAccess::PresentationSceneRevision(*this) != stableRevision
			|| cui::framework::WindowAccess::PresentationNodeCount(*this) != stableNodeCount
			|| cui::framework::WindowAccess::PresentationDrawingLayerCount(*this) != stableLayerCount)
			return fail(L"纯局部 damage 错误地重建了 retained scene topology。");
		if (cui::framework::WindowAccess::PresentationContentRevision(*this) <= stableContentRevision
			|| cui::framework::WindowAccess::PresentationGeometryRevision(*this) != stableGeometryRevision
			|| cui::framework::WindowAccess::PresentationCompositionRevision(*this)
				!= stableCompositionRevision)
			return fail(L"内容局部失效没有单独推进 content revision。");
		const auto contentFrame = cui::framework::WindowAccess::PresentationFrame(*this);
		if (contentFrame.ContentDirtyNodes == 0
			|| contentFrame.CommandRecordedNodes == 0)
			return fail(L"内容局部帧没有记录 dirty node / draw 分类：contentDirty="
				+ std::to_wstring(contentFrame.ContentDirtyNodes)
				+ L"，commandRecorded="
				+ std::to_wstring(contentFrame.CommandRecordedNodes)
				+ L"，frame=" + std::to_wstring(contentFrame.Frame) + L"。");

		const auto geometrySceneRevision = cui::framework::WindowAccess::PresentationSceneRevision(*this);
		const auto contentBeforeGeometry = cui::framework::WindowAccess::PresentationContentRevision(*this);
		const auto geometryBeforeMove = cui::framework::WindowAccess::PresentationGeometryRevision(*this);
		const auto compositionBeforeGeometry =
			cui::framework::WindowAccess::PresentationCompositionRevision(*this);
		const float originalCanvasLeft = Canvas::GetLeft(*(topologyTile));
		Canvas::SetLeft(*(topologyTile), originalCanvasLeft + 24.0f);
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		const auto geometryFrame = cui::framework::WindowAccess::PresentationFrame(*this);
		const auto sceneAfterGeometry =
			cui::framework::WindowAccess::PresentationSceneRevision(*this);
		const auto contentAfterGeometry =
			cui::framework::WindowAccess::PresentationContentRevision(*this);
		const auto geometryAfterMove =
			cui::framework::WindowAccess::PresentationGeometryRevision(*this);
		const auto compositionAfterGeometry =
			cui::framework::WindowAccess::PresentationCompositionRevision(*this);
		if (sceneAfterGeometry != geometrySceneRevision
			|| contentAfterGeometry != contentBeforeGeometry
			|| geometryAfterMove <= geometryBeforeMove
			|| compositionAfterGeometry != compositionBeforeGeometry
			|| geometryFrame.GeometryDirtyNodes == 0
			|| geometryFrame.GeometryRecomputedNodes == 0
			|| geometryFrame.CommandRecordedNodes == 0)
			return fail(L"布局移动没有独立命中 geometry revision/cache 分类："
				L"scene=" + std::to_wstring(geometrySceneRevision)
				+ L"→" + std::to_wstring(sceneAfterGeometry)
				+ L"，content=" + std::to_wstring(contentBeforeGeometry)
				+ L"→" + std::to_wstring(contentAfterGeometry)
				+ L"，geometry=" + std::to_wstring(geometryBeforeMove)
				+ L"→" + std::to_wstring(geometryAfterMove)
				+ L"，composition=" + std::to_wstring(
					compositionBeforeGeometry)
				+ L"→" + std::to_wstring(compositionAfterGeometry)
				+ L"，dirty/recomputed/recorded="
				+ std::to_wstring(geometryFrame.GeometryDirtyNodes)
				+ L"/" + std::to_wstring(
					geometryFrame.GeometryRecomputedNodes)
				+ L"/" + std::to_wstring(
					geometryFrame.CommandRecordedNodes)
				+ L"，frame=" + std::to_wstring(geometryFrame.Frame)
				+ L"。");
		Canvas::SetLeft(*(topologyTile), originalCanvasLeft);
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);

		const auto compositionSceneRevision = cui::framework::WindowAccess::PresentationSceneRevision(*this);
		const auto contentBeforeComposition = cui::framework::WindowAccess::PresentationContentRevision(*this);
		const auto geometryBeforeComposition = cui::framework::WindowAccess::PresentationGeometryRevision(*this);
		const auto compositionBeforeCommit =
			cui::framework::WindowAccess::PresentationCompositionRevision(*this);
		topologyTile->InvalidateComposition();
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		const auto compositionFrame = cui::framework::WindowAccess::PresentationFrame(*this);
		if (cui::framework::WindowAccess::PresentationSceneRevision(*this) != compositionSceneRevision
			|| cui::framework::WindowAccess::PresentationContentRevision(*this) != contentBeforeComposition
			|| cui::framework::WindowAccess::PresentationGeometryRevision(*this) != geometryBeforeComposition
			|| cui::framework::WindowAccess::PresentationCompositionRevision(*this)
				<= compositionBeforeCommit
			|| compositionFrame.CompositionDirtyNodes == 0
			|| compositionFrame.CommandCacheHitNodes == 0)
			return fail(L"compositor-only 失效没有独立命中 composition revision。");

		Invalidate(false);
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		const auto replayFrame = cui::framework::WindowAccess::PresentationFrame(*this);
		if (replayFrame.DamageReplayNodes == 0
			|| replayFrame.CommandReplayedNodes == 0
			|| replayFrame.CommandCacheHitNodes == 0)
			return fail(L"完整帧没有区分 unchanged damage replay 与 dirty node。");

		const int originalZIndex = topologyTile->ZIndex;
		topologyTile->ZIndex = originalZIndex + 17;
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		RECT zOrderDirty{};
		bool zOrderWasFull = false;
		const auto zOrderRevision = cui::framework::WindowAccess::PresentationSceneRevision(*this);
		if (zOrderRevision <= stableRevision
			|| cui::framework::WindowAccess::PresentationNodeCount(*this) != stableNodeCount
			|| !cui::framework::WindowAccess::TryGetLastRenderDirtyRect(*this, zOrderDirty, zOrderWasFull)
			|| !zOrderWasFull)
			return fail(L"ZIndex 变化未重建 retained order 或提升完整帧。");

		topologyTile->Visibility = Visibility::Hidden;
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		RECT detachDirty{};
		bool detachWasFull = false;
		const auto detachRevision = cui::framework::WindowAccess::PresentationSceneRevision(*this);
		if (detachRevision <= zOrderRevision
			|| cui::framework::WindowAccess::PresentationNodeCount(*this) + 1 != stableNodeCount
			|| !cui::framework::WindowAccess::TryGetLastRenderDirtyRect(*this, detachDirty, detachWasFull)
			|| !detachWasFull)
			return fail(L"Visibility=Hidden 未从 retained scene 移除节点或提升完整帧。");

		topologyTile->ZIndex = originalZIndex;
		topologyTile->Visibility = Visibility::Visible;
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		if (cui::framework::WindowAccess::PresentationSceneRevision(*this) <= detachRevision
			|| cui::framework::WindowAccess::PresentationNodeCount(*this) != stableNodeCount
			|| cui::framework::WindowAccess::PresentationDrawingLayerCount(*this) != stableLayerCount)
			return fail(L"retained scene 节点恢复后快照未回到稳定结构。");

		// Deterministic loss uses the production recovery path: the host rebuilds
		// every surface, advances generation, notifies behaviors, invalidates old
		// command lists and commits a complete replacement frame.
		const auto generationBeforeRecovery =
			cui::framework::WindowAccess::PresentationResourceGeneration(*this);
		const auto recoveryBefore = cui::framework::WindowAccess::PresentationDeviceRecoveryCount(*this);
		const auto committedBeforeRecovery =
			cui::framework::WindowAccess::PresentationCommittedFrameCount(*this);
		const auto behaviorInvalidationsBefore =
			behavior->DeviceResourceInvalidations();
		cui::framework::WindowAccess::InjectPresentationDeviceLossForTesting(
			*this);
		(void)::SendMessageW(Handle, WM_PAINT, 0, 0);
		const auto recoveryFrame = cui::framework::WindowAccess::PresentationFrame(*this);
		if (cui::framework::WindowAccess::PresentationResourceGeneration(*this)
			<= generationBeforeRecovery
			|| cui::framework::WindowAccess::PresentationDeviceRecoveryCount(*this) <= recoveryBefore
			|| cui::framework::WindowAccess::PresentationCommittedFrameCount(*this)
				<= committedBeforeRecovery
			|| behavior->DeviceResourceInvalidations()
				<= behaviorInvalidationsBefore
			|| recoveryFrame.ResourceGeneration
				!= cui::framework::WindowAccess::PresentationResourceGeneration(*this)
			|| recoveryFrame.CommandCacheInvalidatedNodes == 0
			|| recoveryFrame.CommandRecordedNodes == 0
			|| recoveryFrame.CommandReplayedNodes == 0)
			return fail(L"注入设备丢失未统一推进 generation、资源通知、命令重录与事务提交。");

		// Exercise the real RichTextBox visual-line virtualization path in a live
		// Window. The first paragraph is intentionally longer than the complete
		// virtualization threshold, so scrolling to the tail cannot rely on a
		// full-paragraph resident DirectWrite layout.
		_tabs->SelectedIndex = containerTabIndex;
		std::wstring largeRichText;
		largeRichText.reserve(26000);
		for (int word = 0; word < 3600; ++word)
			largeRichText.append(L"office ");
		largeRichText.append(L"\r\ntail");
		richText->Text = largeRichText;
		richText->Select(0, 62);
		if (!richText->GetSelection().ApplyPropertyValue(
			TextElement::FontSizeProperty(), BindingValue(40.0)))
		{
			return fail(L"RichTextBox 大文档段落格式化失败。");
		}
		richText->Select(static_cast<int>(largeRichText.size()), 0);
		SetKeyboardFocus(richText, false);
		RequestLayout();
		UpdateLayout();
		Invalidate(false);
		if (!drainPresentationWork())
			return fail(L"RichTextBox 大文档虚拟布局未完成稳定绘制。");
		D2D1_RECT_F richCaret{};
		if (richText->Text != largeRichText
			|| richText->GetCaretIndex()
				!= static_cast<int>(largeRichText.size())
			|| !richText->TryGetTextInputCaretRect(richCaret)
			|| !std::isfinite(richCaret.left)
			|| !std::isfinite(richCaret.top)
			|| !std::isfinite(richCaret.right)
			|| !std::isfinite(richCaret.bottom)
			|| richCaret.right <= richCaret.left
			|| richCaret.bottom <= richCaret.top)
		{
			return fail(L"RichTextBox 大文档虚拟布局、滚动或 caret 投影不完整。");
		}

		if (outError) outError->clear();
		return true;
	}
	catch (const std::exception& error)
	{
		return fail(L"Presentation 特性验证异常："
			+ Convert::StringToWString(error.what()));
	}
}

bool DemoWindow::VerifyRuntimeDataFeatures(std::wstring* outError)
{
	auto fail = [&](std::wstring message)
	{
		if (outError) *outError = std::move(message);
		return false;
	};
	try
	{
		auto* list = RequireControl<ListView>(L"demoList");
		if (!_runtimeDataInitialized
			|| list->ItemCount() != 40)
			return fail(L"非平台运行时数据路径未完整填充 ListView。");
		auto* grid = RequireControl<DataGrid>(L"demoDataGrid");
		const int previousPage = _tabs ? _tabs->SelectedIndex : -1;
		if (!_tabs || !_tabs->SelectItem(PageIndex(DemoPage::DataGrid)))
			return fail(L"无法切换到 DataGrid 页执行运行时编辑验证。");
		RequestLayout();
		UpdateLayout();
		auto* row = dynamic_cast<DataGridRow*>(grid->GetGeneratedItem(1));
		auto* cell = row ? row->GetCell(1) : nullptr;
		auto* amountCell = row ? row->GetCell(5) : nullptr;
		auto* paidCell = row ? row->GetCell(6) : nullptr;
		auto* paidDisplay = paidCell
			? dynamic_cast<CheckBox*>(paidCell->GetVisualContent()) : nullptr;
		if (!paidDisplay
			|| paidDisplay->VerticalAlignment != VerticalAlignment::Top)
			return fail(L"DataGrid CheckBox 显示元素未采用 WPF 默认 Top 对齐。");
		const int namedCurrentCellEvents = _dataGridCurrentCellEvents;
		const int namedEditEvents = _dataGridEditEndingEvents;
		const int namedSortEvents = _dataGridSortEvents;
		if (!row || !cell || !grid->SetCurrentCell(1, 1))
			return fail(L"DataGrid 未生成可编辑的真实 Row/Cell 容器。");
		auto* currentItemState = RequireControl<Label>(
			L"dataGridCurrentItemState");
		auto* currentColumnState = RequireControl<Label>(
			L"dataGridCurrentColumnState");
		BindingSourceReference projectedCurrentItem;
		DataGridColumn* projectedCurrentColumn = nullptr;
		DataGridCellInfo projectedCurrentCell;
		if (!currentItemState->Tag.TryGet(projectedCurrentItem)
			|| projectedCurrentItem.Shared() != row->GetItem().Shared()
			|| !currentColumnState->Tag.TryGet(projectedCurrentColumn)
			|| projectedCurrentColumn != grid->GetColumn(1)
			|| !dataGridStatus->Tag.TryGet(projectedCurrentCell)
			|| projectedCurrentCell.Item.Shared() != row->GetItem().Shared()
			|| projectedCurrentCell.Column != grid->GetColumn(1)
			|| projectedCurrentCell.RowIndex != 1
			|| projectedCurrentCell.ColumnIndex != 1)
			return fail(L"DataGrid CurrentItem/CurrentColumn/CurrentCell MVVM 投影未同步。");
		int beginningEditCount = 0;
		int endingEditCount = 0;
		auto beginningConnection = grid->BeginningEdit.Subscribe(
			[&](DataGrid*, DataGridBeginningEditEventArgs&)
			{ ++beginningEditCount; });
		auto endingConnection = grid->CellEditEnding.Subscribe(
			[&](DataGrid*, DataGridCellEditEndingEventArgs&)
			{ ++endingEditCount; });
		if (!grid->BeginEdit())
			return fail(L"DataGrid TextColumn 无法进入编辑态。");
		auto* editor = dynamic_cast<TextBox*>(cell->GetEditingElement());
		if (!editor)
			return fail(L"DataGridTextColumn 未生成 TextBox 编辑元素。");
		editor->SelectAll();
		editor->InsertText(L"杭州数擎（已编辑）");
		if (!grid->CommitEdit())
			return fail(L"DataGrid TextColumn 提交编辑失败。");
		BindingSourceReference editedItem;
		BindingValue editedCustomer;
		if (!grid->GetItemsSource().Get()->TryGetItem(1, editedItem)
			|| !editedItem
			|| !editedItem.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Customer"), editedCustomer)
			|| editedCustomer.ToString() != L"杭州数擎（已编辑）"
			|| beginningEditCount != 1 || endingEditCount != 1
			|| _dataGridCurrentCellEvents != namedCurrentCellEvents + 1
			|| _dataGridEditEndingEvents != namedEditEvents + 1)
			return fail(L"DataGrid TwoWay 编辑未写回行数据或编辑事件未闭环。");
		if (!grid->BeginEdit())
			return fail(L"DataGrid 提交后无法再次进入编辑态验证取消。");
		editor = dynamic_cast<TextBox*>(cell->GetEditingElement());
		if (!editor)
			return fail(L"DataGrid 取消验证缺少 TextBox 编辑元素。");
		editor->SelectAll();
		editor->InsertText(L"该值必须被取消");
		if (!grid->CancelEdit())
			return fail(L"DataGrid 取消编辑失败。");
		BindingValue customerAfterCancel;
		if (!editedItem.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Customer"), customerAfterCancel)
			|| customerAfterCancel.ToString() != L"杭州数擎（已编辑）"
			|| endingEditCount != 2
			|| _dataGridEditEndingEvents != namedEditEvents + 2)
			return fail(L"DataGrid Esc/CancelEdit 未保留提交前的源值。");

		BindingValue originalAmount;
		long long originalAmountValue = 0;
		if (!editedItem.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Amount"), originalAmount)
			|| !originalAmount.TryGetInt64(originalAmountValue))
			return fail(L"DataGrid 金额编辑验证无法读取原始 Int64 值。");
		if (!amountCell || !grid->SetCurrentCell(1, 5) || !grid->BeginEdit())
			return fail(L"DataGrid 金额编辑模板无法进入编辑态。");
		auto* amountEditor = dynamic_cast<TextBox*>(
			amountCell->GetEditingElement());
		if (!amountEditor
			|| amountEditor->Text != std::to_wstring(originalAmountValue))
			return fail(L"DataGrid 金额编辑模板未使用可回写的原始 Int64 文本。");
		const long long updatedAmountValue = originalAmountValue + 1000;
		amountEditor->SelectAll();
		amountEditor->InsertText(std::to_wstring(updatedAmountValue));
		if (!grid->CommitEdit())
			return fail(L"DataGrid 金额编辑模板无法提交并退出编辑态。");
		row = dynamic_cast<DataGridRow*>(grid->GetGeneratedItem(1));
		amountCell = row ? row->GetCell(5) : nullptr;
		paidCell = row ? row->GetCell(6) : nullptr;
		if (!amountCell || amountCell->GetIsEditing())
			return fail(L"DataGrid 金额编辑模板无法提交并退出编辑态。");
		BindingValue editedAmount;
		long long editedAmountValue = 0;
		if (!editedItem.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"Amount"), editedAmount)
			|| !editedAmount.TryGetInt64(editedAmountValue)
			|| editedAmountValue != updatedAmountValue)
			return fail(L"DataGrid 金额编辑模板未把 Int64 写回源数据。");
		if (!paidCell || !grid->GetColumn(6)->GetIsReadOnly())
			return fail(L"DataGrid 一击切换 CheckBox 模板必须保持列只读编辑事务。");

		auto* orderColumn = grid->GetColumn(0);
		if (!orderColumn || !grid->PerformSort(*orderColumn, false)
			|| !grid->PerformSort(*orderColumn, false)
			|| orderColumn->GetSortDirection()
				!= CollectionSortDirection::Descending
			|| _dataGridSortEvents != namedSortEvents + 2)
			return fail(L"DataGrid 表头升序/降序排序状态未闭环。");
		auto* firstSortedRow = dynamic_cast<DataGridRow*>(
			grid->GetGeneratedItem(0));
		const auto firstSorted = firstSortedRow
			? firstSortedRow->GetItem() : BindingSourceReference{};
		BindingValue firstOrderNumber;
		if (!firstSorted
			|| !firstSorted.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"OrderNo"), firstOrderNumber)
			|| firstOrderNumber.ToString() != L"SO-260818")
			return fail(L"DataGrid 降序排序未更新实际 CollectionView 投影。");

		auto* millionButton = RequireControl<Button>(L"dataGridMillionButton");
#if defined(NDEBUG)
		constexpr bool enforceMillionRowPerformanceBudget = true;
#else
		constexpr bool enforceMillionRowPerformanceBudget = false;
#endif
		const auto fillStarted = std::chrono::steady_clock::now();
		const bool millionInstalled = millionButton->Invoke();
		const auto fillElapsed = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - fillStarted).count();
		if (!millionInstalled
			|| grid->ItemCount() != MillionOrderList::RowCount
			|| grid->GeneratedItemCount() == 0
			|| grid->GeneratedItemCount() >= 512
			|| millionButton->GetContent().ToString() != L"恢复 18 行示例"
			|| !dataGridStatus
			|| dataGridStatus->Text.find(L"1,000,000") == std::wstring::npos)
			return fail(L"DataGrid 百万行按钮未安装按需虚拟化数据源或未发布耗时状态。");
		if (enforceMillionRowPerformanceBudget && fillElapsed >= 1'500.0)
			return fail(L"DataGrid 百万行数据源安装超出稀疏预算："
				+ std::to_wstring(fillElapsed) + L" ms。");
		BindingSourceReference millionTail;
		BindingValue millionTailOrder;
		if (!grid->GetItemsSource().Get()->TryGetItem(
				MillionOrderList::RowCount - 1, millionTail)
			|| !millionTail
			|| !millionTail.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"OrderNo"), millionTailOrder)
			|| millionTailOrder.ToString() != L"LOAD-1000000")
			return fail(L"DataGrid 百万行数据源不支持尾行随机访问。");
		if (!grid->ScrollIntoView(
				BindingValue(millionTail), grid->GetColumn(0)))
			return fail(L"DataGrid ScrollIntoView 无法组合定位百万行尾项与目标列。");
		grid->UpdateLayout();
		if (!grid->GetGeneratedItem(MillionOrderList::RowCount - 1))
			return fail(L"DataGrid ScrollIntoView 未按需实现百万行尾项。");
		auto* millionView = dynamic_cast<CollectionViewSource*>(
			grid->GetItemsSource().Get());
		auto* millionSource = millionView ? dynamic_cast<MillionOrderList*>(
			millionView->GetSource().Get()) : nullptr;
		const auto sortStarted = std::chrono::steady_clock::now();
		if (!millionSource
			|| !grid->PerformSort(*orderColumn, false)
			|| !grid->PerformSort(*orderColumn, false)
			|| orderColumn->GetSortDirection()
				!= CollectionSortDirection::Descending)
			return fail(L"DataGrid 百万行源端排序未闭环。");
		BindingSourceReference descendingFirst;
		BindingValue descendingFirstOrder;
		if (!grid->GetItemsSource().Get()->TryGetItem(0, descendingFirst)
			|| !descendingFirst
			|| !descendingFirst.Get()->TryGetValue(
				MakeBindingSourcePropertyToken(L"OrderNo"),
				descendingFirstOrder)
			|| descendingFirstOrder.ToString() != L"LOAD-1000000"
			|| millionSource->MaterializedCount() >= 512)
			return fail(L"DataGrid 百万行排序退化为全量记录物化。");
		const auto sortElapsed = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - sortStarted).count();
		if (enforceMillionRowPerformanceBudget && sortElapsed >= 1'500.0)
			return fail(L"DataGrid 百万行源端排序超出稀疏预算："
				+ std::to_wstring(sortElapsed) + L" ms。");

		auto* customerColumn = grid->GetColumn(1);
		BindingSourceReference editedZero;
		BindingSourceReference editedOne;
		const auto customerToken =
			MakeBindingSourcePropertyToken(L"Customer");
		if (!customerColumn
			|| !grid->GetItemsSource().Get()->TryGetItem(1, editedZero)
			|| !grid->GetItemsSource().Get()->TryGetItem(2, editedOne)
			|| !editedZero || !editedOne
			|| !editedZero.Get()->TrySetValue(
				customerToken, BindingValue(L"0"))
			|| !editedOne.Get()->TrySetValue(
				customerToken, BindingValue(L"1")))
			return fail(L"DataGrid 百万行编辑后排序验证无法写回 Customer。");
		editedZero = {};
		editedOne = {};
		auto customerAt = [&](size_t index)
		{
			BindingSourceReference item;
			BindingValue value;
			return grid->GetItemsSource().Get()->TryGetItem(index, item)
				&& item && item.Get()->TryGetValue(customerToken, value)
				? value.ToString() : std::wstring{};
		};
		if (!grid->PerformSort(*customerColumn, false)
			|| customerColumn->GetSortDirection()
				!= CollectionSortDirection::Ascending
			|| customerAt(0) != L"0" || customerAt(1) != L"1")
			return fail(L"DataGrid 百万行升序仍使用编辑前的 Customer 值。");
		if (!grid->PerformSort(*customerColumn, false)
			|| customerColumn->GetSortDirection()
				!= CollectionSortDirection::Descending
			|| customerAt(MillionOrderList::RowCount - 2) != L"1"
			|| customerAt(MillionOrderList::RowCount - 1) != L"0")
			return fail(L"DataGrid 百万行降序仍使用编辑前的 Customer 值。");
		if (!millionButton->Invoke()
			|| !grid->GetItemsSource()
			|| grid->GetItemsSource().Get()->Count() != 18
			|| grid->ItemCount() != 19
			|| !grid->GetCanUserAddRows()
			|| millionButton->GetContent().ToString() != L"填充 100 万行")
			return fail(L"DataGrid 百万行按钮无法恢复声明式 18 行示例。");
		if (previousPage >= 0) (void)_tabs->SelectItem(previousPage);
		if (!_media || std::abs(_media->Volume - 0.8) > 0.001)
			return fail(L"MediaElement 运行时数据初始化未执行。");
		if (outError) outError->clear();
		return true;
	}
	catch (const std::exception& error)
	{
		return fail(L"运行时数据验证异常："
			+ Convert::StringToWString(error.what()));
	}
}

void DemoWindow::PrepareRuntimeData()
{
	auto makeFile = [](const wchar_t* name, const wchar_t* kind)
	{
		auto item = std::make_shared<ObservableObject>();
		(void)item->DefineProperty(L"Name", std::wstring(name));
		(void)item->DefineProperty(L"Kind", std::wstring(kind));
		return item;
	};
	auto makeFolder = [&](const wchar_t* name,
		std::initializer_list<std::pair<const wchar_t*, const wchar_t*>> files)
	{
		auto children = std::make_shared<ObservableBindingList>(
			MakeDataTypeToken(L"DemoFile"));
		for (const auto& [fileName, kind] : files)
			children->Items.push_back(BindingSourceReference(
				makeFile(fileName, kind)));
		auto folder = std::make_shared<ObservableObject>();
		(void)folder->DefineProperty(L"Name", std::wstring(name));
		(void)folder->DefineProperty(
			L"Children", BindingListReference(children));
		return folder;
	};

	_treeRoots = std::make_shared<ObservableBindingList>(
		MakeDataTypeToken(L"DemoFolder"));
	_treeRoots->Items.push_back(BindingSourceReference(makeFolder(
		L"Workspace", { { L"DemoWindow.cui.xaml", L"XAML" },
			{ L"DemoWindow.cpp", L"C++" }, { L"DemoTheme.xaml", L"XAML" } })));
	_treeRoots->Items.push_back(BindingSourceReference(makeFolder(
		L"Runtime", { { L"ComponentDefinition", L"Type" },
			{ L"NativeSurface", L"Render" }, { L"BindingList", L"Data" } })));
	_listViewEntries = std::make_shared<ObservableBindingList>(
		MakeDataTypeToken(L"DemoListEntry"));
	for (int index = 0; index < 40; ++index)
	{
		auto entry = std::make_shared<ObservableObject>();
		(void)entry->DefineProperty(
			L"Name", StringHelper::Format(L"List item %02d", index + 1));
		(void)entry->DefineProperty(
			L"State", std::wstring(index % 3 == 0 ? L"Ready" : L"Queued"));
		_listViewEntries->Items.push_back(BindingSourceReference(entry));
	}
	auto makeWpfPerson = [](const wchar_t* first, const wchar_t* last,
		const wchar_t* role)
	{
		auto person = std::make_shared<ObservableObject>();
		(void)person->DefineProperty(L"First", std::wstring(first));
		(void)person->DefineProperty(L"Last", std::wstring(last));
		(void)person->DefineProperty(L"Role", std::wstring(role));
		return person;
	};
	_wpfLabPeople = std::make_shared<ObservableBindingList>(
		MakeDataTypeToken(L"WpfLabPerson"));
	_wpfLabPeople->Items.push_back(BindingSourceReference(
		makeWpfPerson(L"Ada", L"Lovelace", L"Math")));
	_wpfLabPeople->Items.push_back(BindingSourceReference(
		makeWpfPerson(L"Grace", L"Hopper", L"Compiler")));
	_wpfLabSettings = std::make_shared<ObservableObject>();
	(void)_wpfLabSettings->DefineProperty(
		L"accent.color", std::wstring(L"settings[indexer]"));
	(void)_wpfLabSettings->DefineProperty(
		L"title", std::wstring(L"runtime data"));
	_dataContext = std::make_shared<ObservableObject>();
	if (!_dataContext->DefineProperty(
		L"TreeRoots", BindingListReference(_treeRoots), true, false, true))
		ThrowRuntimeError(L"无法创建 TreeRoots DataContext。 ");
	if (!_dataContext->DefineProperty(
		L"DemoListEntries", BindingListReference(_listViewEntries),
		true, false, true))
		ThrowRuntimeError(L"无法创建 DemoListEntries DataContext。 ");
	BindingSourcePropertyMetadata nullableMetadata;
	nullableMetadata.ValueKind = BindingValueKind::String;
	nullableMetadata.ValueType = std::type_index(typeid(std::wstring));
	if (!_dataContext->DefineProperty(
		MakeBindingSourcePropertyToken(L"WpfNullable"),
		std::move(nullableMetadata), BindingValue{}))
		ThrowRuntimeError(L"无法创建 WpfNullable DataContext。 ");
	if (!_dataContext->DefineProperty(L"WpfFirst", std::wstring(L"Ada"))
		|| !_dataContext->DefineProperty(L"WpfLast", std::wstring(L"Lovelace"))
		|| !_dataContext->DefineProperty(
			L"WpfPadded", std::wstring(L"  WPF runtime  "))
		|| !_dataContext->DefineProperty(L"WpfStatus", std::wstring(L"Idle"))
		|| !_dataContext->DefineProperty(L"WpfIsAdmin", false)
		|| !_dataContext->DefineProperty(
			L"WpfPeople", BindingListReference(_wpfLabPeople), true, false, true)
		|| !_dataContext->DefineProperty(
			L"WpfSettings", BindingSourceReference(_wpfLabSettings)))
		ThrowRuntimeError(L"无法创建 WPF 语义实验 DataContext。 ");

}

void DemoWindow::AttachStaticBehaviors()
{
	if (!featureCard)
		ThrowRuntimeError(L"AOT FeatureCard 未生成强类型实例。");
	(void)featureCard->ApplyTemplate();
	if (!featureCard->GetPART_State()
		|| !featureCard->GetPART_Content()
		|| !featureCard->GetPART_Actions()
		|| !featureCard->PublishState(L"Typed C++ attached · generated API"))
		ThrowRuntimeError(L"无法连接 AOT FeatureCard 强类型契约。");
	_generatedEventConnections.emplace_back(
		featureCard->OnMouseDoubleClick.Subscribe(
			[this](Control*, MouseEventArgs& args)
			{
				if (args.ChangedButton != MouseButton::Left) return;
				++_featureInputCount;
				(void)featureCard->PublishState(StringHelper::Format(
					L"Typed input #%d @ %d,%d",
					_featureInputCount, args.X, args.Y));
				args.Handled = true;
			}));

	auto* scene = RequireControl<NativeSurface>(L"demoScene");
	scene->SetBehavior(std::make_unique<DemoSceneBehavior>());
	auto* presentation =
		RequireControl<NativeSurface>(L"presentationProbeSurface");
	presentation->SetBehavior(std::make_unique<PresentationProbeBehavior>());

	// handledEventsToo is an AddHandler concern, so it remains a small piece of
	// application behavior rather than an event attribute in generated XAML.
	_generatedEventConnections.emplace_back(
		wpfRouteOuter->OnMouseDown.Subscribe(
			std::bind_front(&DemoWindow::HandleRouteOuterBubble, this), true));
}
void DemoWindow::ResolveControls()
{
	_menu = RequireControl<Menu>(L"mainMenu");
	_toolBar = RequireControl<ToolBar>(L"mainToolBar");
	_statusBar = RequireControl<StatusBar>(L"mainStatusBar");
	_globalProgress = RequireControl<Slider>(L"globalProgress");
	_statusText = RequireControl<Label>(L"statusText");
	_tabs = RequireControl<TabControl>(L"mainTabs");
	_basicButton = RequireControl<Button>(L"basicButton");
	_dialogCancelButton = RequireControl<Button>(L"dialogCancelButton");
	_radioA = RequireControl<RadioButton>(L"radioA");
	_radioB = RequireControl<RadioButton>(L"radioB");
	_image = RequireControl<Image>(L"demoImage");
	_progress = RequireControl<ProgressBar>(L"demoProgress");
	_progressRing = RequireControl<ProgressRing>(L"progressRing");
	_chart = RequireControl<ChartView>(L"salesChart");
	_toastMessage = RequireControl<Label>(L"toastMessage");
	_systemContextMenu = RequireControl<ContextMenu>(L"systemContextMenu");
	_web = RequireControl<WebBrowser>(L"webBrowser");
	_media = RequireControl<MediaElement>(L"mediaElement");
	_mediaProgress = RequireControl<Slider>(L"mediaProgress");
	_mediaTime = RequireControl<Label>(L"mediaTime");
	_mediaSpeedText = RequireControl<Label>(L"mediaSpeedText");
}

void DemoWindow::InitializeBasicPage()
{
	(void)RequireControl<ComboBox>(L"basicCombo");
}

void DemoWindow::InitializeContainerPage()
{
	(void)RequireControl<ListBox>(L"sideNavigationList");
}

void DemoWindow::InitializeDataPage()
{
	auto* tree = RequireControl<TreeView>(L"demoTree");
	if (!tree->GetItemsSource())
		ThrowRuntimeError(L"demoTree 未绑定 XAML ItemsSource。");

	(void)RequireControl<ListBox>(L"demoListBox");

	(void)RequireControl<ListView>(L"demoList");

}

void DemoWindow::InitializeDataGridPage()
{
	auto* grid = RequireControl<DataGrid>(L"demoDataGrid");
	if (!grid->GetItemsSource()
		|| grid->ColumnCount() != DemoDataGridExpectedColumnCount)
		ThrowRuntimeError(
			L"demoDataGrid 未安装声明式 Columns 或静态 ItemsSource。");
	auto* generatedRegionColumn = dynamic_cast<DataGridTextColumn*>(
		grid->GetColumn(DemoDataGridExpectedColumnCount - 1));
	if (!generatedRegionColumn
		|| !generatedRegionColumn->GetIsAutoGenerated()
		|| generatedRegionColumn->GetHeader().ToString() != L"AOT 区域")
		ThrowRuntimeError(L"demoDataGrid 未安装生成期 AOT 区域列。");
	auto* authoredView = dynamic_cast<CollectionViewSource*>(
		grid->GetItemsSource().Get());
	if (!authoredView || authoredView->Groups().empty()
		|| !grid->GetGroupStyle())
		ThrowRuntimeError(
			L"demoDataGrid 未安装区域分组 CollectionViewSource/GroupStyle。");
	if (grid->GetCellStyle() != L"OrderGridCellStyle"
		|| grid->GetColumnHeaderStyle() != L"OrderGridColumnHeaderStyle"
		|| grid->GetRowStyle() != L"OrderGridRowStyle"
		|| grid->GetRowHeaderStyle() != L"OrderGridRowHeaderStyle"
		|| !grid->GetRowHeaderTemplate()
		|| !grid->GetCanUserResizeRows()
		|| std::abs(grid->GetMinRowHeight() - 30.0) > 0.001
		|| !grid->GetAreRowDetailsFrozen()
		|| grid->GetRowDetailsVisibilityMode()
			!= DataGridRowDetailsVisibilityMode::VisibleWhenSelected
		|| !grid->GetRowDetailsTemplate())
		ThrowRuntimeError(
			L"demoDataGrid 未安装 Grid 级容器样式/行头或行详情模板。");
	const auto authoredRowStyle = grid->GetRowStyle();
	const auto authoredRowHeaderTemplate = grid->GetRowHeaderTemplate();
	const auto authoredRowDetailsTemplate = grid->GetRowDetailsTemplate();
	auto* customerColumn = dynamic_cast<DataGridTextColumn*>(
		grid->GetColumn(1));
	if (!customerColumn
		|| customerColumn->GetHeaderStyle() != L"OrderCustomerHeaderStyle"
		|| customerColumn->GetCellStyle() != L"OrderCustomerCellStyle"
		|| !customerColumn->GetHeaderTemplate())
		ThrowRuntimeError(L"demoDataGrid 客户列未覆盖 Grid 级样式/表头模板。");
	auto* paidColumn = dynamic_cast<DataGridTemplateColumn*>(
		grid->GetColumn(6));
	if (!paidColumn || !paidColumn->GetCellTemplate())
		ThrowRuntimeError(L"demoDataGrid 已付款模板列无效。");
	const auto paidTemplate = paidColumn->GetCellTemplate();
	const ItemTemplateReference unpaidTemplate(
		std::make_shared<DemoUnpaidOrderTemplate>());
	paidColumn->SetCellTemplateSelector(ItemTemplateSelectorReference(
		std::make_shared<DemoPaidTemplateSelector>(
			paidTemplate, unpaidTemplate)));
	paidColumn->SetCellTemplate({});
	grid->SetRowStyle({});
	grid->SetRowStyleSelector(ItemStyleSelectorReference(
		std::make_shared<DemoOrderRowStyleSelector>()));
	grid->SetRowHeaderTemplateSelector(ItemTemplateSelectorReference(
		std::make_shared<DemoPaidTemplateSelector>(
			authoredRowHeaderTemplate, unpaidTemplate)));
	grid->SetRowHeaderTemplate({});
	grid->SetRowDetailsTemplateSelector(ItemTemplateSelectorReference(
		std::make_shared<DemoPaidTemplateSelector>(
			authoredRowDetailsTemplate, unpaidTemplate)));
	grid->SetRowDetailsTemplate({});
	if (authoredRowStyle != L"OrderGridRowStyle"
		|| !grid->GetRowStyleSelector()
		|| !grid->GetRowHeaderTemplateSelector()
		|| !grid->GetRowDetailsTemplateSelector())
		ThrowRuntimeError(L"demoDataGrid 未安装运行时行/详情选择器。");
	const auto authoredItems = grid->GetItemsSource();
	auto editableItems = std::make_shared<ObservableBindingList>(
		MakeDataTypeToken(L"DemoOrder"));
	for (size_t index = 0; index < authoredItems.Get()->Count(); ++index)
	{
		BindingSourceReference item;
		if (!authoredItems.Get()->TryGetItem(index, item) || !item)
			ThrowRuntimeError(L"demoDataGrid 静态订单数据包含无效记录。");
		editableItems->Items.push_back(std::move(item));
	}
	auto nextOrder = std::make_shared<size_t>(editableItems->Count() + 1);
	editableItems->SetNewItemFactory([nextOrder]
	{
		auto row = std::make_shared<ObservableObject>();
		const std::wstring orderNumber = StringHelper::Format(
			L"SO-NEW-%03llu",
			static_cast<unsigned long long>((*nextOrder)++));
		if (!row->DefineProperty(
				L"OrderNo", orderNumber, true, false, true)
			|| !row->DefineProperty(
				L"DetailsUri", L"https://example.test/orders/" + orderNumber,
				true, false, true)
			|| !row->DefineProperty(L"Customer", std::wstring(L"新客户"))
			|| !row->DefineProperty(L"Region", std::wstring(L"未分配"))
			|| !row->DefineProperty(L"Stage", std::wstring(L"待确认"))
			|| !row->DefineProperty(L"Quantity", 1)
			|| !row->DefineProperty(L"Amount", int64_t{ 0 })
			|| !row->DefineProperty(L"Paid", false))
			return BindingSourceReference{};
		return BindingSourceReference(std::move(row));
	});
	authoredView->SetIsLiveGroupingRequested(true);
	authoredView->SetSource(BindingListReference(editableItems));
	if (!authoredView->CanAddNew() || !authoredView->CanRemove())
		ThrowRuntimeError(L"demoDataGrid 可编辑运行时视图未暴露新增/删除能力。");
	// The authored static source is immutable, so construction initially coerces
	// these capabilities off. Re-evaluate them after the view adopts its editable
	// runtime source without replacing the generated view/grouping identity.
	(void)grid->CoerceValue(DataGrid::CanUserAddRowsProperty());
	(void)grid->CoerceValue(DataGrid::CanUserDeleteRowsProperty());
	if (!grid->GetCanUserAddRows() || !grid->GetCanUserDeleteRows())
		ThrowRuntimeError(L"demoDataGrid 未在可编辑运行时视图上启用新增/删除。");
	_dataGridDefaultItems = authoredItems.Shared();
	_dataGridMillionMode = false;
}

void DemoWindow::InitializeAnalyticsPage()
{
	(void)RequireControl<ListView>(L"analyticsRows");
}

void DemoWindow::InitializeSystemPage()
{
	_taskbar = std::make_unique<Taskbar>(Handle);
	_notify = std::make_unique<NotifyIcon>();
	if (!_notify->TrySetIcon(LoadIcon(nullptr, IDI_APPLICATION))
		|| !_notify->TrySetToolTip(L"CUI XAML Demo"))
		ThrowRuntimeError(L"无法预配置 NotifyIcon 图标或提示。");
	auto showWindow = NotifyIconMenuItem(
		L"显示窗口",
		RoutedCommand(L"Demo.System.ShowWindow"),
		std::wstring(L"tray-show"));
	showWindow.CommandTarget = this;
	auto about = NotifyIconMenuItem(
		L"关于命令模型",
		RoutedCommand(L"Demo.System.About"),
		std::wstring(L"tray-about"));
	about.CommandTarget = this;
	auto windowMenu = NotifyIconMenuItem::Submenu(L"窗口命令");
	(void)windowMenu.AddItem(std::move(showWindow));
	(void)windowMenu.AddItem(std::move(about));
	if (!_notify->TryAddMenuItem(std::move(windowMenu))
		|| !_notify->TryAddMenuSeparator())
		ThrowRuntimeError(L"无法预配置 NotifyIcon 窗口命令菜单。");
	auto exit = NotifyIconMenuItem(
		L"退出",
		RoutedCommand(L"Demo.File.Exit"),
		std::wstring(L"tray-exit"));
	exit.CommandTarget = this;
	if (!_notify->TryAddMenuItem(std::move(exit)))
		ThrowRuntimeError(L"无法预配置 NotifyIcon 退出命令。");

	_toastMessage->Text = L"通知视觉树由 XAML 创建；C++ 只更新消息内容。";
}

bool DemoWindow::EnsureNotifyIconInitialized(bool show)
{
	if (!_notify || !Handle || !::IsWindow(Handle)) return false;
	if (!_notify->IsInitialized()
		&& !_notify->TryInitialize(*this)) return false;
	return !show || _notify->IsVisible() || _notify->TryShow();
}

void DemoWindow::InitializeWebPage()
{
	_web->RegisterJsInvokeHandler(L"native.echo", [](const std::wstring& payload)
	{
		return L"echo: " + payload;
	});
	_web->RegisterJsInvokeHandler(L"native.time", [](const std::wstring&)
	{
		SYSTEMTIME time{};
		GetLocalTime(&time);
		return StringHelper::Format(L"%02d:%02d:%02d", time.wHour, time.wMinute, time.wSecond);
	});
	_web->SetHtml(
		LR"html(<!doctype html><html><head><meta charset='utf-8'>
<style>body{font-family:Segoe UI;padding:22px;background:#20252d;color:#eef2f6}
button{padding:9px 14px;border-radius:7px;border:1px solid #5f6b7a;background:#2f7df0;color:white}
.box{margin-top:14px;padding:14px;background:#303641;border-radius:8px}</style></head>
<body><h2>CUI WebBrowser hosted by dynamic XAML</h2>
<p>控件布局来自 <code>DemoWindow.cui.xaml</code>，HTML 和 JS bridge 由 C++ 注入。</p>
<button onclick="window.CUI.invoke('native.time','').then(x=>out.textContent=x)">JS → C++ 获取时间</button>
<div class='box'>输出：<span id='out'>(none)</span></div>
<div class='box'>C++ → JS：<span id='native'>(none)</span></div>
<script>window.setFromNative=x=>(native.textContent=String(x),'ok')</script></body></html>)html");
}

void DemoWindow::InitializeMediaPage()
{
	_media->Volume = 0.8;
}

void DemoWindow::UpdateStatus(const std::wstring& text)
{
	if (_statusText)
	{
		_statusText->Text = text;
		_statusText->InvalidateVisual();
	}
	(void)SetStatusBarItemText(0, text);
}

std::wstring DemoWindow::GetStatusBarItemText(size_t index) const
{
	if (!_statusBar || !_statusBar->GetItemsSource()) return {};
	BindingSourceReference item;
	if (!_statusBar->GetItemsSource().Get()->TryGetItem(index, item) || !item)
		return {};
	BindingValue value;
	std::wstring text;
	return item.Get()->TryGetValue(
		MakeBindingSourcePropertyToken(L"Text"), value)
		&& value.TryGetString(text)
		? text : std::wstring{};
}

bool DemoWindow::SetStatusBarItemText(
	size_t index, const std::wstring& text)
{
	if (!_statusBar || !_statusBar->GetItemsSource()) return false;
	BindingSourceReference item;
	if (!_statusBar->GetItemsSource().Get()->TryGetItem(index, item) || !item)
		return false;
	return item.Get()->TrySetValue(
		MakeBindingSourcePropertyToken(L"Text"), BindingValue(text));
}

std::wstring DemoWindow::DescribeCommandTarget(const Control* target) const
{
	if (!target) return L"∅";
	if (target == this) return L"CuiXamlDemo";
	static constexpr const wchar_t* namedTargets[] = {
		L"systemSurface", L"mainMenu", L"basicButton",
		L"wpfHierarchyScope", L"wpfDispatcherProbe", L"featureCard",
		L"commandTargetButton"
	};
	for (const auto* name : namedTargets)
	{
		if (FindGeneratedControlByName(name) == target) return name;
	}
	return L"(unnamed)";
}

void DemoWindow::RecordCommandTarget(
	bool executed, const RoutedEventArgs& args, const RoutedCommand& command,
	std::uint64_t transactionId)
{
	const auto targetName = DescribeCommandTarget(
		args.OriginalSource ? args.OriginalSource : args.Source);
	if (executed)
	{
		_lastCommandExecutedTarget = targetName;
		_lastCommandTargetCommand = command.Name();
		_lastCommandExecutedTransactionId = transactionId;
		_displayedCommandCanExecuteTarget =
			_pendingCommandTransactionId == transactionId
			&& _pendingCommandTargetCommand == command.Name()
			? _lastCommandCanExecuteTarget : std::wstring(L"∅");
	}
	else
	{
		_lastCommandCanExecuteTarget = targetName;
		_pendingCommandTargetCommand = command.Name();
		_pendingCommandTransactionId = transactionId;
		// CanExecute may run for every command source during a global requery.
		// Keep that query transaction pending, but do not mutate presentation
		// state until the matching command is actually executed.
		return;
	}
	auto* trace = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"commandTargetTrace"));
	if (!trace) return;
	trace->Text = L"CanExecute target="
		+ (_displayedCommandCanExecuteTarget.empty()
			? std::wstring(L"∅") : _displayedCommandCanExecuteTarget)
		+ L" · Executed target="
		+ (_lastCommandExecutedTarget.empty()
			? std::wstring(L"∅") : _lastCommandExecutedTarget)
		+ L" · " + _lastCommandTargetCommand;
}

void DemoWindow::UpdateProgress(float value01)
{
	value01 = std::clamp(value01, 0.0f, 1.0f);
	_progress->Value = value01;
	_progressRing->Value = static_cast<double>(value01) * 100.0;
	if (_taskbar)
		(void)_taskbar->TrySetValue(
			static_cast<ULONGLONG>(value01 * 1000.0f), 1000);
}

void DemoWindow::LoadImage(const std::wstring& path)
{
	if (path.empty()) return;
	const auto extension = StringHelper::ToLower(
		Convert::WStringToString(std::filesystem::path(path).extension().wstring()));
	_image->Source = nullptr;
	if (extension == ".svg")
	{
		const auto svg = File::ReadAllText(Convert::WStringToString(path));
		_image->Source = D2DGraphics::ToBitmapFromSvg(svg.c_str());
	}
	else
		_image->Source = BitmapSource::FromFile(path);
	UpdateStatus(L"Image: " + FileNameFromPath(path));
	Invalidate();
}

void DemoWindow::HandleContentRendered(Window*)
{
	if (_taskbar)
	{
		(void)_taskbar->Initialize(Handle);
		if (_progress)
			(void)_taskbar->TrySetValue(static_cast<ULONGLONG>(
				std::clamp(_progress->Value, 0.0, 1.0) * 1000.0),
				1000);
	}
	const bool notifyReady = !_notify || EnsureNotifyIconInitialized(true);
	UpdateStatus(notifyReady
		? L"AOT XAML 已构造：运行时仅执行原生 C++ 控件树"
		: L"AOT XAML 已构造：NotifyIcon 等待有效 Window Handle");
}

void DemoWindow::HandleClosing(Window* sender, CancelEventArgs& args)
{
	const auto result = MessageDialog::Show(
		L"确认", L"是否关闭 CUI XAML 示例？",
		MessageDialogButtons::YesNo, MessageDialogIcon::Question, sender);
	args.Cancel = result != MessageDialogResult::Yes;
}

void DemoWindow::HandleCommandPreviewCanExecute(
	Control*, CanExecuteRoutedEventArgs& args)
{
	++_commandPreviewCanExecuteCount;
	_commandRouteTrace.push_back(
		L"PreviewCanExecute:" + args.Command.Name());
}

void DemoWindow::HandleCommandCanExecute(
	Control*, CanExecuteRoutedEventArgs& args)
{
	++_commandCanExecuteCount;
	RecordCommandTarget(
		false, args, args.Command, args.CommandTransactionId);
	_commandRouteTrace.push_back(L"CanExecute:" + args.Command.Name());
	args.CanExecute = args.Command.Name() == L"Demo.System.CopyInfo"
		? _copyInfoCommandEnabled : true;
}

void DemoWindow::HandleCommandPreviewExecuted(
	Control*, ExecutedRoutedEventArgs& args)
{
	++_commandPreviewExecutedCount;
	_commandRouteTrace.push_back(L"PreviewExecuted:" + args.Command.Name());
}

void DemoWindow::HandleCommandExecuted(
	Control*, ExecutedRoutedEventArgs& args)
{
	++_commandExecutedCount;
	RecordCommandTarget(
		true, args, args.Command, args.CommandTransactionId);
	_commandRouteTrace.push_back(L"Executed:" + args.Command.Name());
	_lastCommandParameter = CommandParameterText(args.Parameter);
	const auto& command = args.Command.Name();
	if (command == L"Demo.File.Open")
		UpdateStatus(L"RoutedCommand: Open · " + _lastCommandParameter);
	else if (command == L"Demo.File.Exit")
	{
		args.Executed = true;
		if (Handle) Close();
		return;
	}
	else if (command == L"Demo.Help.About")
	{
		args.Executed = true;
		if (Handle)
			MessageDialog::Show(L"关于",
				L"命令 identity、CommandBinding 和 InputBinding 均由 XAML 定义。",
				MessageDialogButtons::OK, MessageDialogIcon::Info, this);
		return;
	}
	else if (command == L"Demo.System.NewProject")
		UpdateStatus(L"ContextMenu RoutedCommand: NewProject");
	else if (command == L"Demo.System.Refresh")
		UpdateStatus(L"ContextMenu RoutedCommand: Refresh · "
			+ _lastCommandParameter);
	else if (command == L"Demo.System.CopyInfo")
		UpdateStatus(L"ContextMenu RoutedCommand: CopyInfo");
	else if (command == L"Demo.System.About")
		UpdateStatus(L"ContextMenu RoutedCommand: About");
	else if (command == L"Demo.System.ShowWindow")
	{
		UpdateStatus(L"NotifyIcon RoutedCommand: ShowWindow · "
			+ _lastCommandParameter);
		if (Handle)
		{
			(void)::ShowWindow(Handle, SW_SHOWNORMAL);
			(void)::SetForegroundWindow(Handle);
		}
	}
	args.Executed = true;
}

void DemoWindow::HandleLocalCommandCanExecute(
	Control*, CanExecuteRoutedEventArgs& args)
{
	++_localCommandCanExecuteCount;
	_commandRouteTrace.push_back(L"LocalCanExecute:" + args.Command.Name());
	args.CanExecute = true;
}

void DemoWindow::HandleLocalCommandExecuted(
	Control*, ExecutedRoutedEventArgs& args)
{
	++_localCommandExecutedCount;
	_lastCommandParameter = CommandParameterText(args.Parameter);
	_commandRouteTrace.push_back(L"LocalExecuted:" + args.Command.Name());
	UpdateStatus(L"Local CommandBinding: " + args.Command.Name()
		+ L" · " + _lastCommandParameter);
	args.Executed = true;
}

void DemoWindow::HandleClassCommandCanExecute(
	Control*, CanExecuteRoutedEventArgs& args)
{
	++_classCommandCanExecuteCount;
	_classCommandTrace.push_back(L"QName.CanExecute");
	args.CanExecute = _classCommandEnabled;
}

void DemoWindow::HandleClassCommandExecuted(
	Control*, ExecutedRoutedEventArgs& args)
{
	++_classCommandExecutedCount;
	_classCommandTrace.push_back(L"QName.Executed");
	_lastCommandParameter = CommandParameterText(args.Parameter);
	UpdateStatus(L"FeatureCard class command · " + _lastCommandParameter);
	args.Executed = true;
}

void DemoWindow::HandleNativeClassCommandCanExecute(
	Control*, CanExecuteRoutedEventArgs& args)
{
	++_nativeClassCommandCanExecuteCount;
	_classCommandTrace.push_back(L"NativeFrameworkElement.CanExecute");
	if (CommandParameterText(args.Parameter) == L"template-native-input")
		args.CanExecute = true;
}

void DemoWindow::HandleNativeClassCommandExecuted(
	Control*, ExecutedRoutedEventArgs& args)
{
	++_nativeClassCommandExecutedCount;
	_classCommandTrace.push_back(L"NativeFrameworkElement.Executed");
	_lastCommandParameter = CommandParameterText(args.Parameter);
	args.Executed = true;
}

void DemoWindow::HandleCommandAvailabilityToggle(
	Control*, RoutedEventArgs&)
{
	_classCommandEnabled = !_classCommandEnabled;
	if (auto* source = FindGeneratedControlByName(
		L"featureActionB"))
		(void)RoutedCommandManager::InvalidateRequerySuggested(*source);
	UpdateStatus(_classCommandEnabled
		? L"FeatureCard class command: enabled (requery pending)"
		: L"FeatureCard class command: disabled (requery pending)");
}

void DemoWindow::HandleToolBarAction(Control* sender, RoutedEventArgs&)
{
	struct NavigationTarget
	{
		const wchar_t* Name;
		DemoPage Page;
		const wchar_t* Label;
	};
	for (const auto& target : {
		NavigationTarget{ L"toolBasic", DemoPage::Basic, L"基础" },
		NavigationTarget{ L"toolData", DemoPage::Data, L"数据" },
		NavigationTarget{ L"toolAnalytics", DemoPage::Analytics, L"可视化" },
		NavigationTarget{ L"toolAnimation", DemoPage::Animation, L"动画" },
		NavigationTarget{ L"toolSystem", DemoPage::System, L"系统" } })
	{
		if (sender != RequireControl<Control>(target.Name)) continue;
		(void)_tabs->SelectItem(PageIndex(target.Page));
		UpdateStatus(std::wstring(L"ToolBar: ") + target.Label);
		return;
	}
	const wchar_t* icons[] = { L"toolIcon1", L"toolIcon2", L"toolIcon3" };
	for (size_t index = 0; index < std::size(icons); ++index)
	{
		if (sender != RequireControl<Control>(icons[index])) continue;
		UpdateStatus(StringHelper::Format(
			L"ToolBar icon %d", static_cast<int>(index) + 1));
		return;
	}
}

void DemoWindow::HandleGlobalProgress(
	Control*, RoutedPropertyChangedEventArgs<double>& e)
{
	const auto value = e.NewValue;
	UpdateProgress(value / 1000.0f);
	UpdateStatus(StringHelper::Format(L"XAML Slider Value=%.0f", value));
}

void DemoWindow::HandleMouseWheel(Control*, MouseEventArgs& e)
{
	UpdateStatus(StringHelper::Format(
		L"MouseWheel Delta=%d", e.WheelDelta));
}

void DemoWindow::HandleBasicClick(Control* sender, RoutedEventArgs&)
{
	if (auto* button = dynamic_cast<Button*>(sender))
	{
		int invocationCount = 0;
		(void)sender->Tag.TryGetInt(invocationCount);
		sender->Tag = BindingValue(invocationCount + 1);
		button->SetContent(BindingValue(StringHelper::Format(
			L"点击计数 [%d]", invocationCount + 1)));
	}
	sender->InvalidateVisual();
	UpdateStatus(L"Button.Click -> HandleBasicClick");
}

void DemoWindow::HandleTemplateSwap(Control* sender, RoutedEventArgs&)
{
	auto* button = dynamic_cast<Button*>(sender);
	if (!button) return;
	BindingValue primaryValue;
	BindingValue alternateValue;
	ControlTemplateReference primary;
	ControlTemplateReference alternate;
	if (!button->TryFindResource(L"WpfLabButtonTemplate", primaryValue)
		|| !button->TryFindResource(
			L"WpfLabButtonTemplateAlternate", alternateValue)
		|| !primaryValue.TryGet(primary)
		|| !alternateValue.TryGet(alternate))
	{
		UpdateStatus(L"Control.Template swap: resource resolution failed");
		return;
	}
	const bool useAlternate = button->GetTemplate() == primary;
	button->SetTemplate(useAlternate ? alternate : primary);
	const bool rebuilt = button->ApplyTemplate();
	UpdateStatus(
		rebuilt
			? useAlternate
				? L"Control.Template → alternate · same Button host"
				: L"Control.Template → primary · same Button host"
			: L"Control.Template swap failed: " + button->LastTemplateError());
}

void DemoWindow::HandleEnableInput(Control* sender, RoutedEventArgs&)
{
	auto* target = RequireControl<Control>(L"nameInput");
	target->IsEnabled = static_cast<CheckBox*>(sender)->IsChecked;
	target->InvalidateVisual();
}

void DemoWindow::HandleRadio(Control* sender, RoutedEventArgs&)
{
	UpdateStatus(sender == _radioA ? L"Radio: A" : L"Radio: B");
}

void DemoWindow::HandleComboSelection(
	Control* sender, SelectionChangedEventArgs&)
{
	UpdateStatus(L"ComboBox: " + static_cast<ComboBox*>(sender)->Text);
}

void DemoWindow::HandleNumericValue(
	Control*, RoutedPropertyChangedEventArgs<double>& e)
{
	UpdateStatus(StringHelper::Format(L"NumericUpDown: %.0f", e.NewValue));
}

void DemoWindow::HandleDocsLink(Control*, RoutedEventArgs&)
{
	UpdateStatus(L"CUI XAML = 设计期编译器 + 生产期原生 C++ 控件树");
}

void DemoWindow::HandleExpander(Control* sender, RoutedEventArgs&)
{
	UpdateStatus(static_cast<Expander*>(sender)->IsExpanded
		? L"Expander: Expanded" : L"Expander: Collapsed");
}

void DemoWindow::HandleOpenImage(Control*, RoutedEventArgs&)
{
	OpenFileDialog dialog;
	dialog.Filter = MakeDialogFilterStrring(
		"图片文件", "*.jpg;*.jpeg;*.png;*.bmp;*.svg;*.webp");
	dialog.SupportMultiDottedExtensions = true;
	dialog.Title = "选择一个图片文件";
	if (dialog.ShowDialog(Handle) == DialogResult::OK && !dialog.SelectedPaths.empty())
		LoadImage(Convert::StringToWString(dialog.SelectedPaths[0]));
}

void DemoWindow::HandleDragRoute(Control*, DragEventArgs& e)
{
	if (e.Data && (e.Data->HasFiles() || e.Data->HasText()))
		e.Effects = DragDropEffects::Copy;
	const auto& metadata = GetRoutedEventMetadata(e.EventId);
	const wchar_t* stage = e.Stage == RoutedEventStage::Preview
		? L"tunnel" : e.Stage == RoutedEventStage::Bubble
			? L"bubble" : L"direct";
	UpdateStatus(StringHelper::Format(
		L"DragDrop.%s · %s · local=(%d,%d) · files=%d · text=%s",
		metadata.Name, stage, e.X, e.Y,
		e.Data ? static_cast<int>(e.Data->Files().size()) : 0,
		e.Data && e.Data->HasText() ? L"yes" : L"no"));
}

void DemoWindow::HandleDropImage(Control*, DragEventArgs& e)
{
	if (!e.Data || !e.Data->HasFiles()) return;
	LoadImage(e.Data->Files().front());
	e.Effects = DragDropEffects::Copy;
	e.Handled = true;
	UpdateStatus(L"Drop · routed DragEventArgs · image loaded · Handled=true");
}

void DemoWindow::HandleImageVisibility(Control* sender, RoutedEventArgs&)
{
	_image->Visibility = static_cast<Switch*>(sender)->IsChecked
		? Visibility::Visible : Visibility::Hidden;
	UpdateStatus(_image->IsVisible
		? L"Image: Visibility=Visible"
		: L"Image: Visibility=Hidden (layout preserved)");
}

void DemoWindow::HandleListViewSelection(
	Control* sender, SelectionChangedEventArgs&)
{
	UpdateStatus(L"ListView: "
		+ static_cast<Selector*>(sender)->GetSelectedValue().ToString());
}

void DemoWindow::HandleListBoxSelection(
	Control* sender, SelectionChangedEventArgs&)
{
	UpdateStatus(L"ListBox: "
		+ static_cast<Selector*>(sender)->GetSelectedValue().ToString());
}

void DemoWindow::HandleTreeSelection(
	Control* sender, RoutedPropertyChangedEventArgs<BindingValue>&)
{
	++_treeSelectionChanges;
	UpdateStatus(L"TreeView data selection: "
		+ static_cast<TreeView*>(sender)->GetSelectedValue().ToString());
}

void DemoWindow::HandleFeatureInvoked(
	Control* sender, DeclarativeEventArgs& args)
{
	++_featureInvocations;
	auto* card = dynamic_cast<DemoWindowGeneratedFeatureCard*>(
		args.OriginalSource ? args.OriginalSource : sender);
	if (card)
		(void)card->PublishState(StringHelper::Format(
			L"Invoked #%d · source handler", _featureInvocations));
	UpdateStatus(L"FeatureCard.Invoked: XAML event -> C++ source handler");
}

void DemoWindow::HandleFeatureBubble(
	Control*, DeclarativeEventArgs& args)
{
	++_featureBubbleInvocations;
	UpdateStatus(StringHelper::Format(
		L"FeatureCard.Invoked bubbled #%d (%s source)",
		_featureBubbleInvocations,
		args.OriginalSource ? L"original" : L"missing"));
}

bool DemoWindow::RunElementHierarchyProbe(std::wstring* outSummary)
{
	auto* target = FindGeneratedControlByName(
		L"wpfTemplateButton");
	auto* result = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"wpfDispatcherResult"));
	auto finish = [&](bool success, std::wstring summary)
	{
		if (result)
		{
			result->Text = summary;
			result->Foreground = success
				? D2D1_COLOR_F{ 0.08f, 0.62f, 0.40f, 1.0f }
				: D2D1_COLOR_F{ 0.86f, 0.22f, 0.22f, 1.0f };
		}
		if (outSummary) *outSummary = std::move(summary);
		return success;
	};
	if (!target || !result)
		return finish(false, L"FAIL · hierarchy probe XAML missing");

	DispatcherObject& dispatcher = *target;
	DependencyObject& dependency = *target;
	Visual& visual = *target;
	UIElement& input = *target;
	FrameworkElement& framework = *target;
	const auto* backgroundProperty = dependency.GetPropertyMetadata(
		Control::BackgroundProperty());
	const bool layerIdentity = dispatcher.CheckAccess()
		&& backgroundProperty && backgroundProperty->Matches(dependency)
		&& backgroundProperty->OwnerType() == std::type_index(typeid(Control))
		&& visual.GetVisualParent() != nullptr
		&& framework.GetLogicalParent() == visual.GetVisualParent()
		&& framework.GetInheritanceParent() == framework.GetLogicalParent()
		&& input.GetActualSizeDip().width > 0.0f
		&& input.GetActualSizeDip().height > 0.0f;

	DependencyObject* changedSender = nullptr;
	const DependencyProperty* changedProperty = nullptr;
	const auto versionBefore = dependency.PropertyChangeVersion();
	auto propertyConnection = dependency.OnPropertyValueChanged.Subscribe(
		[&](DependencyObject* sender,
			const DependencyPropertyChangedEventArgs& args)
		{
			changedSender = sender;
			changedProperty = args.Property;
		});
	const bool propertyWritten = dependency.TrySetPropertyValue(
		Control::AutomationIdProperty(),
		BindingValue(std::wstring(L"wpf-hierarchy-probe")));
	const bool propertyBoundary = propertyWritten
		&& changedSender == &dependency
		&& changedProperty == &Control::AutomationIdProperty()
		&& dependency.PropertyChangeVersion() > versionBefore
		&& target->GetPropertyValueSource(Control::AutomationIdProperty())
			== DependencyPropertyValueSource::Local;
	(void)target->ClearPropertyValue(
		Control::AutomationIdProperty());

	bool foreignCheckAccess = true;
	bool verifyRejected = false;
	bool propertyRejected = false;
	bool renderRejected = false;
	bool postAccepted = false;
	bool postedOnOwner = false;
	std::function<void()> workerBody = [&]
	{
		foreignCheckAccess = dispatcher.CheckAccess();
		try { dispatcher.VerifyAccess(); }
		catch (const std::logic_error&) { verifyRejected = true; }
		try
		{
			(void)dependency.TrySetPropertyValue(
				Control::AutomationIdProperty(),
				BindingValue(std::wstring(L"illegal")));
		}
		catch (const std::logic_error&) { propertyRejected = true; }
		try { visual.InvalidateVisual(); }
		catch (const std::logic_error&) { renderRejected = true; }
		postAccepted = dispatcher.TryPost([&]
		{
			postedOnOwner = dispatcher.CheckAccess();
		});
	};
	HANDLE worker = ::CreateThread(nullptr, 0,
		[](LPVOID context) -> DWORD
		{
			(*static_cast<std::function<void()>*>(context))();
			return 0;
		},
		&workerBody, 0, nullptr);
	if (!worker)
		return finish(false, L"FAIL · cannot create probe worker");
	(void)::WaitForSingleObject(worker, INFINITE);
	::CloseHandle(worker);
	cui::PumpUIThreadCallbacks();

	const bool dispatcherBoundary = !foreignCheckAccess
		&& verifyRejected && propertyRejected && renderRejected
		&& postAccepted && postedOnOwner;
	const bool success = layerIdentity
		&& propertyBoundary && dispatcherBoundary;
	return finish(success, success
		? L"PASS · 6 layers + DP + dispatch"
		: L"FAIL · layer/DP/dispatch boundary [layer="
			+ std::to_wstring(layerIdentity)
			+ L", property=" + std::to_wstring(propertyBoundary)
			+ L", dispatcher=" + std::to_wstring(dispatcherBoundary) + L"]");
}

void DemoWindow::HandleDispatcherProbe(Control*, RoutedEventArgs&)
{
	std::wstring summary;
	const bool success = RunElementHierarchyProbe(&summary);
	UpdateStatus((success ? L"WPF hierarchy: " : L"WPF hierarchy failed: ")
		+ summary);
}

void DemoWindow::HandleRouteOuterPreview(
	Control* sender, MouseEventArgs& e)
{
	_routedInputTrace.clear();
	const wchar_t* changedButton = e.ChangedButton == MouseButton::Left
		? L"Left" : e.ChangedButton == MouseButton::Right
		? L"Right" : e.ChangedButton == MouseButton::Middle
		? L"Middle" : L"Other";
	_routedInputDetail = L"Changed=" + std::wstring(changedButton)
		+ (e.ButtonState == MouseButtonState::Pressed
			? L"/Pressed" : L"/Released")
		+ L" · snapshot "
		+ (e.IsButtonPressed(MouseButton::Left) ? L"L+" : L"L-")
		+ (e.IsButtonPressed(MouseButton::Right) ? L" R+" : L" R-");
	_routedInputTrace.push_back(
		sender == e.CurrentTarget ? L"T outer" : L"T outer(!target)");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRouteMiddlePreview(
	Control* sender, MouseEventArgs& e)
{
	_routedInputTrace.push_back(
		sender == e.CurrentTarget ? L"T middle" : L"T middle(!target)");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRouteSourcePreview(
	Control* sender, MouseEventArgs& e)
{
	auto* source = FindGeneratedControlByName(L"wpfRouteSource");
	_routedInputTrace.push_back(sender == source
		&& e.OriginalSource == source && e.Source == source
		? L"T source" : L"T source(!source)");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRouteSourceBubble(
	Control* sender, MouseEventArgs& e)
{
	_routedInputTrace.push_back(sender == e.CurrentTarget
		? L"B source(H)" : L"B source(!target)");
	e.Handled = true;
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRouteMiddleBubble(
	Control*, MouseEventArgs&)
{
	_routedInputTrace.push_back(L"B middle(!handled)");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRouteOuterBubble(
	Control* sender, MouseEventArgs& e)
{
	_routedInputTrace.push_back(sender == e.CurrentTarget && e.Handled
		? L"B outer(too)" : L"B outer(!handled)");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRouteKey(Control* sender, KeyEventArgs& e)
{
	auto* outer = FindGeneratedControlByName(L"wpfRouteOuter");
	auto* source = FindGeneratedControlByName(L"wpfRouteSource");
	if (e.Stage == RoutedEventStage::Preview && sender == outer)
		_routedInputTrace.clear();
	const wchar_t* stage = e.Stage == RoutedEventStage::Preview ? L"T" : L"B";
	const wchar_t* target = sender == outer ? L"outer"
		: sender == source ? L"source" : L"other";
	_routedInputTrace.push_back(
		L"K." + std::wstring(stage) + L" " + target);
	const auto physicalKey = e.Key == Key::System ? e.SystemKey : e.Key;
	_routedInputDetail = (e.Key == Key::System
		? L"Key=System · SystemKey=" : L"Key=")
		+ FormatKeyGesture(KeyGesture{ physicalKey, e.Modifiers })
		+ (e.IsRepeat ? L" · repeat" : L" · first");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRouteCaptureChanged(
	Control*, RoutedEventArgs& e)
{
	_routedInputTrace.push_back(e.EventId == RoutedEventId::GotMouseCapture
		? L"capture+" : L"capture-");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRouteFocus(Control*, RoutedEventArgs&)
{
	_routedInputTrace.push_back(L"focus+");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRoutePreviewGotKeyboardFocus(
	Control*, KeyboardFocusChangedEventArgs& e)
{
	_lastKeyboardFocusOld = e.OldFocus;
	_lastKeyboardFocusNew = e.NewFocus;
	const bool cancel = _cancelNextKeyboardFocus;
	_cancelNextKeyboardFocus = false;
	_routedInputTrace.push_back(cancel
		? L"keyboard.T+(cancel)" : L"keyboard.T+");
	if (cancel) e.Handled = true;
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRouteGotKeyboardFocus(
	Control*, KeyboardFocusChangedEventArgs& e)
{
	_lastKeyboardFocusOld = e.OldFocus;
	_lastKeyboardFocusNew = e.NewFocus;
	_routedInputTrace.push_back(L"keyboard.B+");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRoutePreviewLostKeyboardFocus(
	Control*, KeyboardFocusChangedEventArgs& e)
{
	_lastKeyboardFocusOld = e.OldFocus;
	_lastKeyboardFocusNew = e.NewFocus;
	_routedInputTrace.push_back(L"keyboard.T-");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleRouteLostKeyboardFocus(
	Control*, KeyboardFocusChangedEventArgs& e)
{
	_lastKeyboardFocusOld = e.OldFocus;
	_lastKeyboardFocusNew = e.NewFocus;
	_routedInputTrace.push_back(L"keyboard.B-");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleTextOuterPreview(
	Control*, TextCompositionEventArgs& e)
{
	_routedInputTrace.clear();
	_routedInputTrace.push_back(L"text.T outer[" + e.Text + L"]");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleTextSourcePreview(
	Control*, TextCompositionEventArgs&)
{
	_routedInputTrace.push_back(L"text.T source");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleTextSourceBubble(
	Control*, TextCompositionEventArgs&)
{
	_routedInputTrace.push_back(L"text.B source");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleTextOuterBubble(
	Control*, TextCompositionEventArgs&)
{
	_routedInputTrace.push_back(L"text.B outer");
	RefreshRoutedInputSummary();
}

void DemoWindow::HandleCompositionPreviewStart(
	Control*, TextCompositionEventArgs& e)
{
	_textCompositionTrace.clear();
	_textCompositionTrace.push_back(
		L"T.Start #" + std::to_wstring(e.CompositionId));
	RefreshTextCompositionSummary();
}

void DemoWindow::HandleCompositionStart(
	Control*, TextCompositionEventArgs& e)
{
	_textCompositionTrace.push_back(
		L"B.Start #" + std::to_wstring(e.CompositionId));
	RefreshTextCompositionSummary();
}

void DemoWindow::HandleCompositionPreviewUpdate(
	Control*, TextCompositionEventArgs& e)
{
	_textCompositionTrace.push_back(
		L"T.Update #" + std::to_wstring(e.CompositionId)
		+ L" [" + e.CompositionText + L"] @"
		+ std::to_wstring(e.CaretIndex));
	RefreshTextCompositionSummary();
}

void DemoWindow::HandleCompositionUpdate(
	Control*, TextCompositionEventArgs& e)
{
	_textCompositionTrace.push_back(
		L"B.Update #" + std::to_wstring(e.CompositionId)
		+ L" [" + e.CompositionText + L"] @"
		+ std::to_wstring(e.CaretIndex));
	RefreshTextCompositionSummary();
}

void DemoWindow::HandleCompositionPreviewCommit(
	Control*, TextCompositionEventArgs& e)
{
	std::wstring token = L"T.Commit #" + std::to_wstring(e.CompositionId)
		+ L" [" + CompositionPayloadText(e) + L"]";
	if (_compositionPreviewHandled)
	{
		token += L" handled";
		e.Handled = true;
	}
	_textCompositionTrace.push_back(std::move(token));
	RefreshTextCompositionSummary();
}

void DemoWindow::HandleCompositionCommit(
	Control*, TextCompositionEventArgs& e)
{
	_textCompositionTrace.push_back(
		L"B.Commit #" + std::to_wstring(e.CompositionId)
		+ (e.TextApplied ? L" applied" : L" not-applied")
		+ (dynamic_cast<PasswordBox*>(e.OriginalSource)
			? L" <secure>" : L""));
	RefreshTextCompositionSummary();
}

void DemoWindow::HandleTextCompositionProbe(
	Control* sender, RoutedEventArgs& e)
{
	auto* textBox = dynamic_cast<TextBox*>(
		FindGeneratedControlByName(L"compositionTextBox"));
	auto* richTextBox = dynamic_cast<RichTextBox*>(
		FindGeneratedControlByName(L"compositionRichTextBox"));
	auto* passwordBox = dynamic_cast<PasswordBox*>(
		FindGeneratedControlByName(L"compositionPasswordBox"));
	if (!sender || !textBox || !richTextBox || !passwordBox) return;
	auto& manager = cui::framework::WindowAccess::TextComposition(*this);
	auto* start = FindGeneratedControlByName(
		L"compositionStartProbe");
	auto* update = FindGeneratedControlByName(
		L"compositionUpdateProbe");
	auto* commit = FindGeneratedControlByName(
		L"compositionCommitProbe");
	auto* cancel = FindGeneratedControlByName(
		L"compositionCancelProbe");
	auto* surrogate = FindGeneratedControlByName(
		L"compositionSurrogateProbe");
	auto* unichar = FindGeneratedControlByName(
		L"compositionUnicharProbe");
	auto* focus = FindGeneratedControlByName(
		L"compositionFocusProbe");
	auto* preview = FindGeneratedControlByName(
		L"compositionPreviewHandledProbe");
	auto* reset = FindGeneratedControlByName(
		L"compositionResetProbe");
	auto startFresh = [&]()
	{
		if (manager.Snapshot().IsComposing)
			manager.CancelComposition(TextCompositionCancelReason::Explicit);
		_textCompositionTrace.clear();
		SetKeyboardFocus(textBox, false);
		return manager.StartComposition(
			textBox, TextCompositionInputKind::Programmatic);
	};
	auto ensureUpdated = [&]()
	{
		const auto snapshot = manager.Snapshot();
		if (!snapshot.IsComposing || snapshot.Source != textBox)
		{
			if (!startFresh()) return false;
		}
		return manager.UpdateComposition(
			L"ni", 2, { 0, 0 }, { 0, 2 });
	};

	if (sender == start)
	{
		(void)startFresh();
		UpdateStatus(L"TextComposition: deterministic Start");
	}
	else if (sender == update)
	{
		(void)ensureUpdated();
		UpdateStatus(L"TextComposition: pre-edit Update(ni)");
	}
	else if (sender == commit)
	{
		if (!manager.Snapshot().IsComposing) (void)ensureUpdated();
		(void)manager.CompleteComposition(L"\u4F60\U0001F600");
		UpdateStatus(L"TextComposition: committed one UTF-16 payload");
	}
	else if (sender == cancel)
	{
		if (!manager.Snapshot().IsComposing) (void)ensureUpdated();
		const auto id = manager.Snapshot().CompositionId;
		manager.CancelComposition(TextCompositionCancelReason::Explicit);
		_textCompositionTrace.push_back(
			L"Manager.Cancel #" + std::to_wstring(id) + L" [explicit]");
		UpdateStatus(L"TextComposition: canceled without a public routed event");
	}
	else if (sender == surrogate)
	{
		if (manager.Snapshot().IsComposing)
			manager.CancelComposition(TextCompositionCancelReason::Explicit);
		_textCompositionTrace.clear();
		SetKeyboardFocus(textBox, false);
		(void)manager.ProcessWindowMessage(
			WM_CHAR, static_cast<WPARAM>(0xD83D), 0);
		(void)manager.ProcessWindowMessage(
			WM_CHAR, static_cast<WPARAM>(0xDE00), 0);
		UpdateStatus(L"TextComposition: surrogate pair committed once");
	}
	else if (sender == unichar)
	{
		if (manager.Snapshot().IsComposing)
			manager.CancelComposition(TextCompositionCancelReason::Explicit);
		_textCompositionTrace.clear();
		SetKeyboardFocus(textBox, false);
		(void)manager.ProcessWindowMessage(WM_UNICHAR, UNICODE_NOCHAR, 0);
		(void)manager.ProcessWindowMessage(
			WM_UNICHAR, static_cast<WPARAM>(0x1F642), 0);
		UpdateStatus(L"TextComposition: WM_UNICHAR scalar committed");
	}
	else if (sender == focus)
	{
		(void)startFresh();
		(void)manager.UpdateComposition(L"focus", 5);
		const auto id = manager.Snapshot().CompositionId;
		SetKeyboardFocus(
			richTextBox, false, FocusChangeReason::Programmatic);
		const auto snapshot = manager.Snapshot();
		_textCompositionTrace.push_back(
			L"Manager.Cancel #" + std::to_wstring(id) + L" ["
			+ CompositionCancelText(snapshot.CancelReason) + L"]");
		UpdateStatus(L"TextComposition: focus transaction resolved before commit");
	}
	else if (sender == preview)
	{
		_compositionPreviewHandled = !_compositionPreviewHandled;
		if (auto* button = dynamic_cast<Button*>(preview))
			button->SetContent(BindingValue(_compositionPreviewHandled
				? L"Preview Handled: on" : L"Preview Handled: off"));
		UpdateStatus(_compositionPreviewHandled
			? L"TextComposition: preview will suppress default edit"
			: L"TextComposition: preview allows default edit");
	}
	else if (sender == reset)
	{
		manager.Reset();
		_compositionPreviewHandled = false;
		_textCompositionTrace.clear();
		textBox->Text = L"TextBox: ";
		textBox->Select(static_cast<int>(textBox->Text.size()), 0);
		richTextBox->Text = L"RichTextBox:\r\n";
		richTextBox->Select(static_cast<int>(richTextBox->Text.size()), 0);
		passwordBox->Password = L"";
		passwordBox->Select(0, 0);
		if (auto* button = dynamic_cast<Button*>(preview))
			button->SetContent(BindingValue(
				std::wstring(L"Preview Handled: off")));
		SetKeyboardFocus(textBox, false);
		UpdateStatus(L"TextComposition experiment reset");
	}

	e.Handled = true;
	RefreshTextCompositionSummary();
}

void DemoWindow::RefreshTextCompositionSummary()
{
	auto* state = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"compositionState"));
	auto* statistics = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"compositionStats"));
	auto* trace = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"compositionTrace"));
	if (!state || !statistics || !trace) return;
	const auto snapshot = cui::framework::WindowAccess::TextCompositionState(*this);
	const auto stats = cui::framework::WindowAccess::TextCompositionStatisticsOf(*this);
	std::wstring source = L"\u2205";
	if (snapshot.Source == FindGeneratedControlByName(
		L"compositionTextBox")) source = L"TextBox";
	else if (snapshot.Source == FindGeneratedControlByName(
		L"compositionRichTextBox")) source = L"RichTextBox";
	else if (snapshot.Source == FindGeneratedControlByName(
		L"compositionPasswordBox")) source = L"PasswordBox";
	else if (snapshot.Source) source = L"other-client";
	state->Text = CompositionStageText(snapshot.Stage)
		+ L" · " + CompositionKindText(snapshot.InputKind)
		+ L" · id " + std::to_wstring(snapshot.CompositionId)
		+ L" · caret " + std::to_wstring(snapshot.CaretIndex)
		+ L" · source " + source;
	if (snapshot.Stage == TextCompositionStage::Canceled)
		state->Text += L" · " + CompositionCancelText(snapshot.CancelReason);
	statistics->Text =
		L"native " + std::to_wstring(stats.NativeReports)
		+ L" · start/update/complete/cancel "
		+ std::to_wstring(stats.CompositionsStarted) + L"/"
		+ std::to_wstring(stats.CompositionsUpdated) + L"/"
		+ std::to_wstring(stats.CompositionsCompleted) + L"/"
		+ std::to_wstring(stats.CompositionsCanceled)
		+ L" · commit/applied/blocked "
		+ std::to_wstring(stats.TextCommits) + L"/"
		+ std::to_wstring(stats.TextApplications) + L"/"
		+ std::to_wstring(stats.PreviewApplicationsSuppressed)
		+ L" · echo " + std::to_wstring(stats.ImeEchoesSuppressed);
	std::wstring timeline;
	for (const auto& token : _textCompositionTrace)
	{
		if (!timeline.empty()) timeline += L"  \u2192  ";
		timeline += token;
	}
	trace->Text = timeline.empty()
		? L"\u7B49\u5F85\u771F\u5B9E IME \u6216\u786E\u5B9A\u6027\u63A2\u9488\u2026" : timeline;
}

void DemoWindow::RefreshRoutedInputSummary()
{
	std::wstring trace;
	for (const auto& token : _routedInputTrace)
	{
		if (!trace.empty()) trace += L" → ";
		trace += token;
	}
	RequireControl<Label>(L"wpfRouteTrace")->Text = trace.empty()
		? L"等待 routed input" : trace;
	const auto stats = cui::framework::WindowAccess::InputStatistics(*this);
	RequireControl<Label>(L"wpfInputStats")->Text =
		L"raw " + std::to_wstring(stats.RawReports)
		+ L" · capture " + std::to_wstring(stats.MouseCaptureAcquired)
		+ L"/" + std::to_wstring(stats.MouseCaptureReleased)
		+ L" · focus " + std::to_wstring(stats.KeyboardFocusTransitions)
		+ L" · skip "
		+ std::to_wstring(stats.HandlersSkippedAfterHandled)
		+ (_routedInputDetail.empty()
			? std::wstring{} : L" · " + _routedInputDetail);
}

void DemoWindow::HandlePresentationRegion(Control*, RoutedEventArgs&)
{
	auto* surface = dynamic_cast<NativeSurface*>(
		FindGeneratedControlByName(L"presentationProbeSurface"));
	auto* behavior = surface ? dynamic_cast<PresentationProbeBehavior*>(
		surface->Behavior()) : nullptr;
	auto* status = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"presentationStatus"));
	if (!surface || !behavior || !status) return;
	behavior->Pulse(*surface);
	status->Text = StringHelper::Format(
		L"Region #%d · frame %d · scene r%llu / %llu nodes",
		behavior->RegionRequests(), behavior->FrameCount(),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationSceneRevision(*this)),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationNodeCount(*this)));
	UpdateStatus(L"PresentationRenderHost: local dirty region queued");
}

void DemoWindow::HandlePresentationGeometry(Control*, RoutedEventArgs&)
{
	auto* tile = FindGeneratedControlByName(
		L"presentationTopologyTile");
	auto* status = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"presentationStatus"));
	if (!tile || !status) return;
	Canvas::SetLeft(*(tile), Canvas::GetLeft(*(tile)) > 40.0f ? 12.0f : 70.0f);
	status->Text = StringHelper::Format(
		L"Geometry move queued · lanes C/G/P %llu/%llu/%llu · topology r%llu stable",
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationContentRevision(*this)),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationGeometryRevision(*this)),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationCompositionRevision(*this)),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationSceneRevision(*this)));
	UpdateStatus(L"PresentationScene: geometry snapshot invalidated without topology rebuild");
}

void DemoWindow::HandlePresentationComposition(Control*, RoutedEventArgs&)
{
	auto* tile = FindGeneratedControlByName(
		L"presentationTopologyTile");
	auto* status = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"presentationStatus"));
	if (!tile || !status) return;
	tile->InvalidateComposition();
	status->Text = StringHelper::Format(
		L"Composition recommit queued · lanes C/G/P %llu/%llu/%llu · no content change",
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationContentRevision(*this)),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationGeometryRevision(*this)),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationCompositionRevision(*this)));
	UpdateStatus(L"PresentationScene: compositor-only revision queued");
}

void DemoWindow::HandlePresentationFullFrame(Control*, RoutedEventArgs&)
{
	auto* surface = dynamic_cast<NativeSurface*>(
		FindGeneratedControlByName(L"presentationProbeSurface"));
	auto* behavior = surface ? dynamic_cast<PresentationProbeBehavior*>(
		surface->Behavior()) : nullptr;
	auto* status = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"presentationStatus"));
	if (!behavior || !status) return;
	behavior->NoteFullFrameRequest();
	status->Text = StringHelper::Format(
		L"Full #%d · scene r%llu / %llu nodes / %llu segments",
		behavior->FullFrameRequests(),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationSceneRevision(*this)),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationNodeCount(*this)),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationDrawingLayerCount(*this)));
	Invalidate(true);
	UpdateStatus(L"PresentationRenderHost: full presentation frame requested");
}

void DemoWindow::HandlePresentationTopology(Control*, RoutedEventArgs&)
{
	auto* tile = FindGeneratedControlByName(
		L"presentationTopologyTile");
	auto* status = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"presentationStatus"));
	if (!tile || !status) return;
	tile->Visibility = tile->IsVisible
		? Visibility::Hidden : Visibility::Visible;
	Invalidate(true);
	status->Text = StringHelper::Format(
		L"Topology node %s · scene r%llu / %llu nodes / %llu segments",
		tile->IsVisible ? L"attached" : L"hidden",
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationSceneRevision(*this)),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationNodeCount(*this)),
		static_cast<unsigned long long>(cui::framework::WindowAccess::PresentationDrawingLayerCount(*this)));
	status->InvalidateVisual();
	UpdateStatus(L"PresentationScene: retained topology snapshot rebuilt");
}

void DemoWindow::HandlePresentationDeviceLoss(Control*, RoutedEventArgs&)
{
	auto* status = dynamic_cast<Label*>(
		FindGeneratedControlByName(L"presentationStatus"));
	if (!status) return;
	const auto generation = cui::framework::WindowAccess::PresentationResourceGeneration(*this);
	const auto recoveries = cui::framework::WindowAccess::PresentationDeviceRecoveryCount(*this);
	cui::framework::WindowAccess::InjectPresentationDeviceLossForTesting(*this);
	status->Text = StringHelper::Format(
		L"Device loss injected · gen %llu → pending · recoveries %llu",
		static_cast<unsigned long long>(generation),
		static_cast<unsigned long long>(recoveries));
	UpdateStatus(
		L"PresentationRenderHost: deterministic device recovery queued");
}

void DemoWindow::HandleAnalyticsAction(Control* sender, RoutedEventArgs&)
{
	auto* query = RequireControl<TextBox>(L"analyticsQuery");
	if (sender == RequireControl<Control>(L"analyticsReset"))
	{
		query->Clear();
		UpdateStatus(L"XAML filter composition: reset");
		return;
	}
	UpdateStatus(query->Text.empty()
		? L"XAML filter composition: apply all"
		: L"XAML filter composition: " + query->Text);
}

void DemoWindow::HandleDataGridSorting(
	DataGrid* grid, DataGridSortingEventArgs& e)
{
	++_dataGridSortEvents;
	if (_dataGridMillionMode && grid && e.Column)
	{
		auto* view = dynamic_cast<CollectionViewSource*>(
			grid->GetItemsSource().Get());
		auto* source = view ? dynamic_cast<MillionOrderList*>(
			view->GetSource().Get()) : nullptr;
		size_t columnIndex = grid->ColumnCount();
		for (size_t index = 0; index < grid->ColumnCount(); ++index)
			if (grid->GetColumn(index) == e.Column)
			{
				columnIndex = index;
				break;
			}
		const auto started = std::chrono::steady_clock::now();
		if (source && source->ApplySort(columnIndex, e.Direction))
		{
			for (size_t index = 0; index < grid->ColumnCount(); ++index)
				if (auto* column = grid->GetColumn(index))
					column->SetSortDirection(
						column == e.Column
							? std::optional(e.Direction) : std::nullopt);
			e.Handled = true;
			const auto elapsed = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - started).count();
			if (dataGridStatus)
				dataGridStatus->Text = StringHelper::Format(
					L"百万行源端索引排序：%.2f ms", elapsed);
			UpdateStatus(StringHelper::Format(
				L"DataGrid: million-row source sort in %.2f ms", elapsed));
			return;
		}
	}
	if (!dataGridStatus) return;
	dataGridStatus->Text = e.Direction == CollectionSortDirection::Ascending
		? L"表头事件：Ascending"
		: L"表头事件：Descending";
}

void DemoWindow::HandleDataGridAddingNewItem(
	DataGrid*, DataGridAddingNewItemEventArgs&)
{
	++_dataGridAddingNewItemEvents;
	if (dataGridStatus)
		dataGridStatus->Text = L"新增行：AddingNewItem";
}

void DemoWindow::HandleDataGridInitializingNewItem(
	DataGrid*, DataGridInitializingNewItemEventArgs& e)
{
	++_dataGridInitializingNewItemEvents;
	if (!dataGridStatus) return;
	dataGridStatus->Text = e.NewItem
		? L"新增行：InitializingNewItem"
		: L"新增行初始化缺少记录";
}

void DemoWindow::HandleDataGridLoadingRow(
	DataGrid*, DataGridRowEventArgs&)
{
	++_dataGridLoadingRowEvents;
}

void DemoWindow::HandleDataGridLoadingRowDetails(
	DataGrid*, DataGridRowDetailsEventArgs& e)
{
	++_dataGridLoadingRowDetailsEvents;
	if (dataGridStatus && e.Row && e.DetailsElement)
		dataGridStatus->Text = L"行详情已加载：R"
			+ std::to_wstring(e.Row->ItemIndex() + 1);
}

void DemoWindow::HandleDataGridRowDetailsVisibilityChanged(
	DataGrid*, DataGridRowDetailsEventArgs& e)
{
	++_dataGridRowDetailsVisibilityEvents;
	if (!dataGridStatus || !e.Row) return;
	dataGridStatus->Text = e.Row->GetDetailsVisibility() == Visibility::Visible
		? L"行详情已展开（水平冻结）"
		: L"行详情已折叠";
}

void DemoWindow::HandleDataGridUnloadingRow(
	DataGrid*, DataGridRowEventArgs&)
{
	++_dataGridUnloadingRowEvents;
}

void DemoWindow::HandleDataGridUnloadingRowDetails(
	DataGrid*, DataGridRowDetailsEventArgs&)
{
	++_dataGridUnloadingRowDetailsEvents;
}

void DemoWindow::HandleDataGridScale(Control* sender, RoutedEventArgs&)
{
	auto* button = dynamic_cast<Button*>(sender);
	auto* grid = RequireControl<DataGrid>(L"demoDataGrid");
	if (!button) return;
	if (!_dataGridDefaultItems)
		_dataGridDefaultItems = grid->GetItemsSource().Shared();

	const auto started = std::chrono::steady_clock::now();
	if (_dataGridMillionMode)
	{
		grid->SetItemsSource(BindingListReference(_dataGridDefaultItems));
		grid->UpdateLayout();
		_dataGridMillionMode = false;
		button->SetContent(BindingValue(L"填充 100 万行"));
		if (dataGridStatus)
			dataGridStatus->Text = L"已恢复声明式示例 · 18 行";
		UpdateStatus(L"DataGrid: restored 18 declarative rows");
		return;
	}

	auto source = std::make_shared<MillionOrderList>();
	auto view = std::make_shared<CollectionViewSource>();
	view->SetSource(BindingListReference(source));
	grid->SetItemsSource(BindingListReference(view));
	grid->UpdateLayout();
	const auto elapsed = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - started).count();
	_dataGridMillionMode = true;
	button->SetContent(BindingValue(L"恢复 18 行示例"));
	if (dataGridStatus)
		dataGridStatus->Text = StringHelper::Format(
			L"1,000,000 行 · 首帧 %.2f ms · 已物化 %llu 行",
			elapsed,
			static_cast<unsigned long long>(source->MaterializedCount()));
	UpdateStatus(StringHelper::Format(
		L"DataGrid: 1,000,000 lazy rows in %.2f ms", elapsed));
}

void DemoWindow::HandleDataGridCellEditEnding(
	DataGrid*, DataGridCellEditEndingEventArgs& e)
{
	++_dataGridEditEndingEvents;
	if (!dataGridStatus) return;
	dataGridStatus->Text = e.EditAction == DataGridEditAction::Commit
		? L"编辑事件：Commit"
		: L"编辑事件：Cancel";
}

void DemoWindow::HandleDataGridRowEditEnding(
	DataGrid*, DataGridRowEditEndingEventArgs& e)
{
	++_dataGridRowEditEndingEvents;
	if (!dataGridStatus) return;
	dataGridStatus->Text = e.EditAction == DataGridEditAction::Commit
		? L"行事务：Commit"
		: L"行事务：Cancel";
}

void DemoWindow::HandleDataGridCurrentCellChanged(
	DataGrid*, DataGridCurrentCellChangedEventArgs& e)
{
	++_dataGridCurrentCellEvents;
	if (!dataGridStatus || !e.NewCell.IsValid()) return;
	dataGridStatus->Text = L"当前单元格：R"
		+ std::to_wstring(e.NewCell.RowIndex + 1) + L" C"
		+ std::to_wstring(e.NewCell.ColumnIndex + 1);
}

void DemoWindow::HandleDataGridSelectedCellsChanged(
	DataGrid* sender, DataGridSelectedCellsChangedEventArgs& e)
{
	if (!dataGridStatus || !sender) return;
	dataGridStatus->Text = L"单元格选择：已选 "
		+ std::to_wstring(sender->GetSelectedCells().size())
		+ L" · 新增 " + std::to_wstring(e.AddedCells.size())
		+ L" · 移除 " + std::to_wstring(e.RemovedCells.size());
}

void DemoWindow::HandleChartKind(Control* sender, RoutedEventArgs&)
{
	if (sender == RequireControl<Control>(L"chartBar"))
		_chart->ChartKind = ChartViewKind::Bar;
	else if (sender == RequireControl<Control>(L"chartPie"))
		_chart->ChartKind = ChartViewKind::Pie;
	else
		_chart->ChartKind = ChartViewKind::Line;
	_chart->ResetView();
	UpdateStatus(L"ChartView: kind changed from XAML button");
}

void DemoWindow::HandleChartPoint(ChartView* sender, int series, int point)
{
	const auto& chartSeries = sender->GetSeries();
	if (series < 0 || series >= static_cast<int>(chartSeries.size())) return;
	if (point < 0 || point >= static_cast<int>(chartSeries[series].Points.size())) return;
	UpdateStatus(StringHelper::Format(L"Chart: %s / %s = %.1f",
		chartSeries[series].Name.c_str(),
		chartSeries[series].Points[point].Label.c_str(),
		chartSeries[series].Points[point].Value));
}

void DemoWindow::HandleFarButton(Control*, RoutedEventArgs&)
{
	UpdateStatus(L"ScrollViewer: reached far XAML child");
}

void DemoWindow::HandleSystemAction(Control* sender, RoutedEventArgs&)
{
	if (sender == RequireControl<Control>(L"notifyToggle"))
	{
		bool changed = false;
		if (_notify && _notify->IsVisible()) changed = _notify->TryHide();
		else changed = EnsureNotifyIconInitialized(true);
		UpdateStatus(changed
			? (_notify->IsVisible() ? L"NotifyIcon: Show" : L"NotifyIcon: Hide")
			: L"NotifyIcon: Window Handle 尚未就绪");
	}
	else if (sender == RequireControl<Control>(L"notifyBalloon"))
	{
		if (EnsureNotifyIconInitialized(true))
			(void)_notify->TryShowBalloonTip(
				L"CUI XAML", L"按钮来自 AOT 生成的 C++ 控件树。", 3000, NIIF_INFO);
		else UpdateStatus(L"NotifyIcon: 无法在当前 Window Handle 上显示气泡");
	}
	else if (sender == RequireControl<Control>(L"showDialog"))
	{
		(void)MessageDialog::Show(L"CUI MessageDialog",
			L"这个按钮及其布局来自 XAML，调用对话框是 C++ 业务行为。",
			MessageDialogButtons::OK, MessageDialogIcon::Info, this);
	}
	else if (sender == RequireControl<Control>(L"dismissToast"))
	{
		_toastMessage->Text = L"暂无通知。";
		UpdateStatus(L"XAML notification composition: cleared");
	}
	else
	{
		_toastMessage->Text = L"运行时业务已更新 XAML 通知内容。";
		UpdateStatus(L"XAML notification composition: updated");
	}
}

void DemoWindow::HandleSystemSurfaceMouseUp(Control* sender, MouseEventArgs& e)
{
	if (e.ChangedButton != MouseButton::Right || !_systemContextMenu) return;
	_systemContextMenu->ShowAt(sender, e.X, e.Y);
	UpdateStatus(L"ContextMenu: shown from XAML Panel.OnMouseUp");
}

void DemoWindow::HandleInvokeWeb(Control*, RoutedEventArgs&)
{
	SYSTEMTIME time{};
	GetLocalTime(&time);
	const auto text = StringHelper::Format(
		L"from C++ at %02d:%02d:%02d", time.wHour, time.wMinute, time.wSecond);
	_web->ExecuteScriptAsync(L"window.setFromNative(" + ToJsStringLiteral(text) + L");");
}

void DemoWindow::HandleNavigationWeb(Control*, RoutedEventArgs&)
{
	_web->Navigate(L"https://www.bing.com");
}

void DemoWindow::HandleMediaCommand(Control* sender, RoutedEventArgs&)
{
	if (sender == RequireControl<Control>(L"mediaOpen"))
	{
		OpenFileDialog dialog;
		dialog.Filter = MakeDialogFilterStrring(
			"媒体文件", "*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.mp3;*.wav;*.flac;*.m4a");
		dialog.SupportMultiDottedExtensions = true;
		dialog.Title = "选择媒体文件";
		if (dialog.ShowDialog(Handle) == DialogResult::OK && !dialog.SelectedPaths.empty())
		{
			_media->Load(Convert::StringToWString(dialog.SelectedPaths[0]));
			_media->Play();
		}
	}
	else if (sender == RequireControl<Control>(L"mediaPlay")) _media->Play();
	else if (sender == RequireControl<Control>(L"mediaPause")) _media->Pause();
	else _media->Stop();
}

void DemoWindow::HandleMediaVolume(
	Control*, RoutedPropertyChangedEventArgs<double>& e)
{
	if (_media) _media->Volume = e.NewValue / 100.0;
}

void DemoWindow::HandleMediaSpeed(
	Control*, RoutedPropertyChangedEventArgs<double>& e)
{
	if (!_media || !_mediaSpeedText) return;
	const auto value = e.NewValue;
	_media->SpeedRatio = value / 100.0f;
	_mediaSpeedText->Text = StringHelper::Format(L"%.2fx", value / 100.0f);
	_mediaSpeedText->InvalidateVisual();
}

void DemoWindow::HandleMediaLoop(Control* sender, RoutedEventArgs&)
{
	if (_media) _media->Loop = static_cast<CheckBox*>(sender)->IsChecked;
}

void DemoWindow::HandleMediaSeek(
	Control*, RoutedPropertyChangedEventArgs<double>& e)
{
	if (_updatingMediaProgress || !_media || _media->Duration <= 0) return;
	_media->Position = e.NewValue / 1000.0 * _media->Duration;
}

void DemoWindow::HandleMediaOpened(Control* senderControl)
{
	auto* sender = static_cast<MediaElement*>(senderControl);
	const int total = static_cast<int>(sender->Duration);
	_mediaTime->Text = StringHelper::Format(
		L"00:00 / %02d:%02d", total / 60, total % 60);
	_mediaTime->InvalidateVisual();
	UpdateStatus(L"MediaElement: " + FileNameFromPath(sender->Source));
}

void DemoWindow::HandleMediaEnded(Control*)
{
	_mediaTime->Text = L"播放结束";
	_mediaTime->InvalidateVisual();
	UpdateStatus(L"MediaElement: Ended");
}

void DemoWindow::HandleMediaFailed(Control*)
{
	_mediaTime->Text = L"加载失败";
	_mediaTime->InvalidateVisual();
	UpdateStatus(L"MediaElement: Failed");
}

void DemoWindow::HandleMediaPosition(Control* senderControl, double position)
{
	auto* sender = static_cast<MediaElement*>(senderControl);
	const int current = static_cast<int>(position);
	const int total = std::max(0, static_cast<int>(sender->Duration));
	_mediaTime->Text = StringHelper::Format(L"%02d:%02d / %02d:%02d",
		current / 60, current % 60, total / 60, total % 60);
	_mediaTime->InvalidateVisual();
	if (sender->Duration > 0)
	{
		_updatingMediaProgress = true;
		_mediaProgress->Value = static_cast<float>(position / sender->Duration * 1000.0);
		_updatingMediaProgress = false;
	}
}
