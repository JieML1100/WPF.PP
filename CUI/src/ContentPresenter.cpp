#include "ContentPresenter.h"

#include "ContentControl.h"
#include "Label.h"
#include "Layout/OverlayLayout.h"
#include "TreeInfrastructure.h"
#include "Window.h"

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <utility>

namespace
{
	/**
	 * Projects WPF-style scalar (including null/Empty) Content through the
	 * existing strongly typed IItemTemplate ABI. The value is an immutable
	 * snapshot: changing Content rebuilds the presenter and therefore creates a
	 * new template data item.
	 */
	class TemplateBindingValueSource final
		: public IBindingSource,
		  public IBindingRootValueProvider
	{
	public:
		explicit TemplateBindingValueSource(BindingValue value)
			: _value(std::move(value))
		{
		}

#if CUI_ENABLE_DYNAMIC_XAML
		bool TryGetValue(
			const std::wstring& propertyName,
			BindingValue& out) const override
		{
			(void)propertyName;
			(void)out;
			return false;
		}

		bool TrySetValue(
			const std::wstring& propertyName,
			const BindingValue& value) override
		{
			(void)propertyName;
			(void)value;
			return false;
		}

		bool TryGetPropertyMetadata(
			const std::wstring& propertyName,
			BindingSourcePropertyMetadata& out) const override
		{
			(void)propertyName;
			(void)out;
			return false;
		}

		std::vector<BindingSourcePropertyMetadata>
			GetProperties() const override
		{
			return {};
		}
#endif

		bool TryGetValue(
			BindingSourcePropertyToken property,
			BindingValue& out) const override
		{
			if (property != BindingRootValuePropertyToken()) return false;
			out = _value;
			return true;
		}

		bool TrySetValue(
			BindingSourcePropertyToken property,
			const BindingValue& value) override
		{
			(void)property;
			(void)value;
			return false;
		}

		bool TryGetPropertyMetadata(
			BindingSourcePropertyToken property,
			BindingSourcePropertyMetadata& out) const override
		{
			if (property != BindingRootValuePropertyToken()) return false;
			out = Metadata();
			return true;
		}

		PropertyChangedEvent& PropertyChanged() override
		{
			return _propertyChanged;
		}

		bool TryGetBindingRootValue(BindingValue& out) const override
		{
			out = _value;
			return true;
		}

		bool TryGetBindingRootValueMetadata(
			BindingSourcePropertyMetadata& out) const override
		{
			out = Metadata();
			return true;
		}

	private:
		BindingSourcePropertyMetadata Metadata() const
		{
#if CUI_ENABLE_DYNAMIC_XAML
			return { std::wstring{}, _value.Kind(),
				std::type_index(_value.Type()), true, false, true };
#else
			return { _value.Kind(), std::type_index(_value.Type()),
				true, false, true };
#endif
		}

		BindingValue _value;
		PropertyChangedEvent _propertyChanged;
	};

	BindingSourceReference ProjectTemplateContent(
		const BindingValue& content)
	{
		BindingSourceReference source;
		if (content.TryGet(source) && source) return source;
		return BindingSourceReference(
			std::make_shared<TemplateBindingValueSource>(content));
	}

	template<typename TValue>
	DependencyPropertyOptions<ContentPresenter, TValue> DataOptions(
		TValue defaultValue
		CUI_DESIGN_METADATA_ARGUMENTS(
			int order,
			DependencyPropertyPersistence persistence =
				DependencyPropertyPersistence::Metadata))
	{
		DependencyPropertyOptions<ContentPresenter, TValue> options;
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

}

const DependencyPropertyMetadataRegistration&
ContentPresenter::ContentPropertyMetadataRelation()
{
	static const DependencyPropertyMetadataRegistration relation = []
	{
		auto options = DataOptions(
			BindingValue{}
			CUI_DESIGN_METADATA_ARGUMENTS(
				10, DependencyPropertyPersistence::Native));
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Browsable = false;
		)
		options.Coerce = [](
			ContentPresenter& target,
			const BindingValue& proposed) -> std::optional<BindingValue>
		{
			std::wstring error;
			try
			{
				if (!target.ValidateContentCandidate(
					proposed, target._contentTemplate, error))
				{
					target._lastTemplateError = std::move(error);
					return std::nullopt;
				}
			}
			catch (...)
			{
				target._lastTemplateError = std::move(error);
				throw;
			}
			return proposed;
		};
		options.Changed = [](
			ContentPresenter& target,
			const BindingValue&, const BindingValue&)
		{
			(void)target.RebuildContent();
		};
		return DependencyPropertyRegistry::AddOwnerStatic<
			ContentPresenter, BindingValue>(ContentControl::ContentProperty(),
			[](ContentPresenter& target) { return target.GetContent(); },
			[](ContentPresenter& target, const BindingValue& value)
			{ target.SetContent(value); }, {}, std::move(options));
	}();
	return relation;
}

const DependencyPropertyMetadataRegistration&
ContentPresenter::ContentTemplatePropertyMetadataRelation()
{
	static const DependencyPropertyMetadataRegistration relation = []
	{
		auto options = DataOptions(
			ItemTemplateReference{}
			CUI_DESIGN_METADATA_ARGUMENTS(
				20, DependencyPropertyPersistence::Native));
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Browsable = false;
		)
		options.Coerce = [](
			ContentPresenter& target,
			const ItemTemplateReference& proposed)
			-> std::optional<ItemTemplateReference>
		{
			std::wstring error;
			try
			{
				if (!target.ValidateContentCandidate(
					target._content, proposed, error))
				{
					target._lastTemplateError = std::move(error);
					return std::nullopt;
				}
			}
			catch (...)
			{
				target._lastTemplateError = std::move(error);
				throw;
			}
			return proposed;
		};
		options.Changed = [](
			ContentPresenter& target,
			const ItemTemplateReference&, const ItemTemplateReference&)
		{
			(void)target.RebuildContent();
		};
		return DependencyPropertyRegistry::AddOwnerStatic<
			ContentPresenter, ItemTemplateReference>(
				ContentControl::ContentTemplateProperty(),
				[](ContentPresenter& target)
				{ return target.GetContentTemplate(); },
				[](ContentPresenter& target,
					const ItemTemplateReference& value)
				{ target.SetContentTemplate(value); }, {}, std::move(options));
	}();
	return relation;
}

const DependencyProperty& ContentPresenter::ContentProperty()
{
	return ContentPropertyMetadataRelation().Property();
}

const DependencyProperty& ContentPresenter::ContentTemplateProperty()
{
	return ContentTemplatePropertyMetadataRelation().Property();
}

ContentPresenter::ContentPresenter()
	: Control()
{
	RendererBackgroundColor = D2D1_COLOR_F{ 0, 0, 0, 0 };
}

void ContentPresenter::OnRender()
{
	if (!IsVisible || !GetPresentationWindow() || !GetDrawingContext()) return;
	if (TryCompleteEmptyRender()) return;
	BeginRender();
	EndRender();
}

cui::core::Size ContentPresenter::MeasureCore(
	const cui::core::Constraints& available)
{
	return cui::layout::MeasureOverlayChildren(
		GetLayoutChildrenView(), available);
}

void ContentPresenter::Arrange(cui::core::Rect finalRect)
{
	Control::Arrange(finalRect);
	PerformPendingLayout();
}

void ContentPresenter::RequestLayout()
{
	_contentLayoutPending = true;
	Control::RequestLayout();
}

void ContentPresenter::OnComputedLayoutSizeChanged()
{
	_contentLayoutPending = true;
}

void ContentPresenter::PerformPendingLayout()
{
	if (IsLayoutSuspended() || !_contentLayoutPending) return;
	const auto size = GetActualSizeDip();
	cui::layout::ArrangeOverlayChildren(
		GetLayoutChildrenView(),
		cui::core::Rect{ 0.0f, 0.0f, size.width, size.height });
	_contentLayoutPending = false;
}

bool ContentPresenter::ValidateVisualChildCollection(
	std::span<Control* const> children,
	std::string& error) const
{
	if (_generatedContentMutationTransaction != 0)
	{
		auto* expected = _generatedContentMutationVisual.Get();
		const auto nonNullCount = std::count_if(
			children.begin(), children.end(),
			[](const Control* child) { return child != nullptr; });
		const bool exactMutation = expected
			? nonNullCount == 1
				&& std::find(children.begin(), children.end(), expected)
					!= children.end()
			: nonNullCount == 0;
		if (!exactMutation)
		{
			error =
				"ContentPresenter generated content transaction mismatch";
			return false;
		}
		return true;
	}
	if (_generatedContent
		&& std::find(children.begin(), children.end(), _generatedContent)
		== children.end())
	{
		error = "ContentPresenter generated content cannot be mutated directly";
		return false;
	}
	size_t authoredCount = 0;
	for (auto* child : children)
		if (child && child != _generatedContent) ++authoredCount;
	if (authoredCount > 1)
	{
		error = "ContentPresenter accepts at most one visual content root";
		return false;
	}
	if (authoredCount != 0
		&& (_generatedContent || !_content.Empty() || _contentTemplate))
	{
		error = "ContentPresenter visual content cannot be combined with data Content";
		return false;
	}
	return true;
}

Control* ContentPresenter::GetVisualContent() const noexcept
{
	Control* result = nullptr;
	auto* mutationVisual = _generatedContentMutationTransaction != 0
		? _generatedContentMutationVisual.Get() : nullptr;
	for (auto* child : GetVisualChildrenView())
	{
		if (!child || child == _generatedContent
			|| child == mutationVisual) continue;
		if (result) return nullptr;
		result = child;
	}
	return result;
}

void ContentPresenter::OnVisualChildCollectionChanged(
	const CollectionChangedEventArgs& change,
	std::span<Control* const> previousChildren)
{
	(void)change;
	(void)previousChildren;
	_contentLayoutPending = true;
}

#if !CUI_ENABLE_DYNAMIC_XAML
void ContentPresenter::RegisterDependencyProperties()
{
	Control::RegisterDependencyProperties();
}
#endif

const DependencyPropertyMetadata*
ContentPresenter::ResolveExactDependencyPropertyMetadata(
	const DependencyProperty& property) const
{
	if (&property == &ContentControl::ContentProperty())
		return &ContentPropertyMetadataRelation().Metadata();
	if (&property == &ContentControl::ContentTemplateProperty())
		return &ContentTemplatePropertyMetadataRelation().Metadata();
	return Control::ResolveExactDependencyPropertyMetadata(property);
}

void ContentPresenter::SetContent(BindingValue value)
{
	if (BindingValuesEqual(_content, value)) return;
	_lastTemplateError.clear();
	(void)SetPropertyField(
		ContentProperty(), _content, std::move(value));
}

void ContentPresenter::SetContentTemplate(ItemTemplateReference value)
{
	if (_contentTemplate == value) return;
	_lastTemplateError.clear();
	(void)SetPropertyField(
		ContentTemplateProperty(), _contentTemplate, std::move(value));
}

void ContentPresenter::SetCompiledDisplayMemberPath(
	CompiledBindingPathView value)
{
	if (value.Version != CompiledBindingPathVersion)
		throw std::invalid_argument(
			"ContentPresenter compiled display path version is unsupported");
	if (_compiledDisplayMemberPath.Version == value.Version
		&& _compiledDisplayMemberPath.Steps.data() == value.Steps.data()
		&& _compiledDisplayMemberPath.Steps.size() == value.Steps.size()
#if CUI_ENABLE_DYNAMIC_XAML
		&& _displayMemberPath.empty()
#endif
		)
		return;
	_compiledDisplayMemberPath = value;
#if CUI_ENABLE_DYNAMIC_XAML
	_displayMemberPath.clear();
#endif
	if (!_contentTemplate) (void)RebuildContent();
}

void ContentPresenter::SetContentTypeToken(DataTypeToken value)
{
	if (_contentTypeToken == value) return;
	const auto previous = _contentTypeToken;
	_contentTypeToken = value;
	if (RebuildContent()) return;
	const auto error = _lastTemplateError;
	_contentTypeToken = previous;
	(void)RebuildContent();
	_lastTemplateError = error;
}

std::wstring ContentPresenter::ReadProjectedDisplayText(
	const BindingSourceReference& source) const
{
	if (!_compiledDisplayMemberPath.Empty())
		return GetBindingRecordText(source, _compiledDisplayMemberPath);
#if CUI_ENABLE_DYNAMIC_XAML
	return ReadAuthoredProjectedDisplayText(source);
#else
	return {};
#endif
}

BindingPathObservation ContentPresenter::ObserveProjectedDisplayPath(
	const BindingSourceReference& source,
	std::function<void()> changed) const
{
	if (!_compiledDisplayMemberPath.Empty())
		return ObserveBindingPaths(
			source, { _compiledDisplayMemberPath }, std::move(changed));
#if CUI_ENABLE_DYNAMIC_XAML
	return ObserveAuthoredProjectedDisplayPath(
		source, std::move(changed));
#else
	return {};
#endif
}

bool ContentPresenter::ValidateContentCandidate(
	const BindingValue& content,
	const ItemTemplateReference& contentTemplate,
	std::wstring& error) const
{
	error.clear();
	if (GetVisualContent())
	{
		if (!content.Empty() || contentTemplate)
		{
			error = L"ContentPresenter 的视觉内容不能与数据内容同时使用。";
			return false;
		}
		return true;
	}
	if (content.Empty() || !contentTemplate) return true;
	// Coercion validates the Content/ContentTemplate contract only. Instantiating
	// the template here would create and immediately destroy a complete visual
	// tree, then Changed/RebuildContent would create it a second time. WPF's
	// FrameworkTemplate path likewise validates metadata before LoadContent and
	// performs exactly one instantiation for the committed value.
	return true;
}

bool ContentPresenter::RebuildContent()
{
	_lastTemplateError.clear();
	auto* inheritedMutationVisual =
		_generatedContentMutationTransaction != 0
		? _generatedContentMutationVisual.Get() : nullptr;
	const ControlWeakReference transactionPrevious(
		_generatedContent ? _generatedContent : inheritedMutationVisual);
	const auto committedAtEntry =
		_generatedContentCommittedTransaction;
	auto transaction = ++_generatedContentTransactionSerial;
	if (transaction == 0)
		transaction = ++_generatedContentTransactionSerial;
	++_generatedContentTransactionDepth;
	struct TransactionDepthGuard final
	{
		size_t& Depth;
		~TransactionDepthGuard() { --Depth; }
	} transactionDepthGuard{ _generatedContentTransactionDepth };
	auto supersededByCommittedRebuild = [&]() noexcept
		{
			return _generatedContentCommittedTransaction
				!= committedAtEntry
				&& _generatedContentCommittedTransaction
					!= transaction;
		};
	auto beginMutation = [&](Control* expected) noexcept
		{
			_generatedContentMutationTransaction = transaction;
			_generatedContentMutationVisual = expected;
		};
	auto endMutation = [&]() noexcept
		{
			if (_generatedContentMutationTransaction != transaction)
				return;
			_generatedContentMutationTransaction = 0;
			_generatedContentMutationVisual.Reset();
		};

	if (GetVisualContent())
	{
		if (!_content.Empty() || _contentTemplate)
		{
			_lastTemplateError =
				L"ContentPresenter 的视觉内容不能与数据内容同时使用。";
			return false;
		}
		return true;
	}
	std::unique_ptr<Control> replacement;
	BindingSourceReference replacementDataContext;
	BindingPathObservation observation;
	if (_contentTemplate)
	{
		BindingSourceReference source;
		const bool hasSource = _content.TryGet(source) && source;
		replacementDataContext = hasSource
			? source : ProjectTemplateContent(_content);
		if (!replacementDataContext)
		{
			_lastTemplateError = L"Content 无法投影为模板数据项。";
			return false;
		}
		replacement = _contentTemplate.Get()->Build(
			replacementDataContext, 0, &_lastTemplateError);
		if (!replacement)
		{
			if (_lastTemplateError.empty())
				_lastTemplateError = L"ContentTemplate 未生成视觉根。";
			return false;
		}
	}
	else if (!_content.Empty())
	{
		BindingSourceReference source;
		const bool hasSource = _content.TryGet(source) && source;
		auto label = std::make_unique<Label>();
		if (hasSource)
			label->Text = ReadProjectedDisplayText(source);
		else label->Text = _content.ToString();
		replacement = std::move(label);
		if (hasSource)
		{
			auto changed = [this]
				{ if (!_contentTemplate) (void)RebuildContent(); };
			observation = ObserveProjectedDisplayPath(
				source, std::move(changed));
		}
	}

	// Template construction and binding setup can themselves run user code.
	// A newer rebuild that committed there owns the current visual transaction;
	// this older invocation must not detach it as its presumed previous value.
	if (supersededByCommittedRebuild())
		return true;

	const ControlWeakReference replacementLifetime(replacement.get());
	const auto previousLifetime = transactionPrevious;
	std::unique_ptr<Control> previous;
	beginMutation(nullptr);
	try
	{
		if (auto* livePrevious = previousLifetime.Get())
		{
			bool ownershipCommittedElsewhere = false;
			std::exception_ptr ignoredNotificationError;
			previous = cui::framework::TreeAccess::DetachVisualChild(
				*this, livePrevious, &ownershipCommittedElsewhere,
				&ignoredNotificationError);
			if (supersededByCommittedRebuild())
			{
				endMutation();
				return true;
			}
			livePrevious = previousLifetime.Get();
			if (livePrevious && (livePrevious->GetVisualParent() == this
				|| IndexOfVisualChild(livePrevious) >= 0))
			{
				_generatedContent = livePrevious;
				throw std::logic_error(
					"ContentPresenter generated content detach did not commit");
			}
		}

		// Do not publish the candidate raw pointer before the ownership
		// transaction commits. Collection validation has an explicit internal
		// transaction path above, so a callback that transfers or destroys the
		// candidate cannot leave _generatedContent dangling.
		_generatedContent = nullptr;
		if (replacement)
		{
			beginMutation(replacementLifetime.Get());
			(void)cui::framework::TreeAccess::
				InsertOwnedVisualChildPreserving(
					*this, VisualChildCount(), replacement, this);
			if (supersededByCommittedRebuild())
			{
				endMutation();
				return true;
			}
			auto* liveReplacement = replacementLifetime.Get();
			if (!liveReplacement
				|| liveReplacement->GetVisualParent() != this
				|| IndexOfVisualChild(liveReplacement) < 0
				|| liveReplacement->GetLogicalParent() != this)
				throw std::logic_error(
					"ContentPresenter generated content attachment did not commit");
			_generatedContent = liveReplacement;
		}
		endMutation();
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		if (supersededByCommittedRebuild())
		{
			endMutation();
			return true;
		}
		auto* liveReplacement = replacementLifetime.Get();
		const bool replacementOwnedByPresenter = liveReplacement
			&& (liveReplacement->GetVisualParent() == this
				|| IndexOfVisualChild(liveReplacement) >= 0);
		_generatedContent = replacementOwnedByPresenter
			? liveReplacement : nullptr;
		if (replacementOwnedByPresenter)
			_generatedContentDataContext = replacementDataContext;

		// A retained replacement owner means insertion failed before commit.
		// In that case restore the previous visual when possible so a rejected
		// rebuild does not unnecessarily blank the presenter.
		if (!replacementOwnedByPresenter && replacement && previous)
		{
			beginMutation(previousLifetime.Get());
			try
			{
				(void)cui::framework::TreeAccess::
					InsertOwnedVisualChildPreserving(
						*this, VisualChildCount(), previous, this);
			}
			catch (...)
			{
				// Preserve the rebuild failure. Final weak/parent validation
				// below still publishes a restoration that committed before a
				// notification failed.
			}
			auto* livePrevious = previousLifetime.Get();
			if (livePrevious
				&& livePrevious->GetVisualParent() == this
				&& IndexOfVisualChild(livePrevious) >= 0)
				_generatedContent = livePrevious;
		}

		// Detach itself may have failed before commit. In that case the old
		// visual never left this presenter and remains the published content.
		if (!_generatedContent)
		{
			auto* livePrevious = previousLifetime.Get();
			if (livePrevious
				&& (livePrevious->GetVisualParent() == this
					|| IndexOfVisualChild(livePrevious) >= 0))
				_generatedContent = livePrevious;
		}
		endMutation();
		std::rethrow_exception(originalError);
	}
	_generatedContentCommittedTransaction = transaction;
	_generatedContentDataContext = std::move(replacementDataContext);
	_contentObservation = std::move(observation);
	RequestLayout();
	InvalidateVisual();
	return true;
}
