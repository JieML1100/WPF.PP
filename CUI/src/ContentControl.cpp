#include "ContentControl.h"
#include "Layout/OverlayLayout.h"
#include "Window.h"
#include "TreeInfrastructure.h"

#include <cwctype>
#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
	Control* AttachOwnedVisualChildPreserving(
		Control& visualParent,
		std::unique_ptr<Control>& value,
		Control* logicalParent)
	{
		if (!value)
			throw std::invalid_argument("不能添加空控件");
		auto* raw = value.release();
		const ControlWeakReference lifetime(raw);
		bool structuralCommit = false;
		try
		{
			cui::framework::TreeAccess::InsertVisualChild(
				visualParent,
				visualParent.VisualChildCount(),
				raw,
				logicalParent,
				&structuralCommit);
		}
		catch (...)
		{
			auto* live = lifetime.Get();
			const bool requestedParentOwns = live
				&& visualParent.IndexOfVisualChild(live) >= 0;
			if (requestedParentOwns
				&& live->GetVisualParent() == &visualParent
				&& live->GetLogicalParent() != logicalParent)
			{
				// Visual-parent notification may throw before the collection
				// synchronizer reaches the logical-parent phase. The structural
				// commit already transferred ownership, so finish publishing
				// the requested logical edge before propagating the observer
				// error.
				try
				{
					cui::framework::TreeAccess::SetLogicalParent(
						*live, logicalParent);
				}
				catch (...) {}
			}
			// Before structural commit, the released pointer is still the
			// caller's object. After commit, a callback may synchronously detach
			// it into another unique_ptr while leaving final VisualParent null;
			// the transaction token is what prevents a second owner here.
			if (live && !structuralCommit && !requestedParentOwns
				&& !live->GetVisualParent())
				value.reset(live);
			throw;
		}

		auto* live = lifetime.Get();
		if (!live)
		{
			throw std::logic_error(
				"ContentControl visual child was destroyed during attachment");
		}
		if (live->GetVisualParent() != &visualParent
			|| visualParent.IndexOfVisualChild(live) < 0
			|| live->GetLogicalParent() != logicalParent)
		{
			if (!structuralCommit && !live->GetVisualParent()
				&& visualParent.IndexOfVisualChild(live) < 0)
				value.reset(live);
			throw std::logic_error(
				"ContentControl visual child attachment did not commit");
		}
		return live;
	}

	std::unique_ptr<Control> DetachOwnedVisualChildPreserving(
		Control& visualParent,
		Control* child,
		std::exception_ptr* notificationError = nullptr)
	{
		if (notificationError) *notificationError = {};
		if (!child) return {};
		const ControlWeakReference lifetime(child);
		bool visualOwnershipCommit = false;
		std::exception_ptr coreNotificationError;
		try
		{
			auto detached =
				cui::framework::TreeAccess::DetachVisualChild(
					visualParent, child, &visualOwnershipCommit,
					&coreNotificationError);
			if (notificationError)
				*notificationError = coreNotificationError;
			auto* released = detached.release();
			if (!released) return {};
			auto* live = lifetime.Get();
			if (!live) return {};
			// A null-parent notification may synchronously attach the raw child
			// to another tree. The returned base unique_ptr is then only a stale
			// transfer token and must never become a second owner.
			if (live->GetVisualParent()
				|| visualParent.IndexOfVisualChild(live) >= 0)
				return {};
			return std::unique_ptr<Control>(live);
		}
		catch (...)
		{
			// DetachVisualChildCore converts every post-commit observer failure
			// into a returned owner plus notificationError. An exception here
			// is therefore pre-commit and the original visual parent still owns
			// the child.
			throw;
		}
	}

	bool SetLogicalParentPreservingOwnership(
		std::unique_ptr<Control>& value,
		Control* logicalParent,
		std::exception_ptr* notificationError = nullptr)
	{
		if (notificationError) *notificationError = {};
		if (!value) return false;
		auto* raw = value.release();
		const ControlWeakReference lifetime(raw);
		bool visualOwnershipCommit = false;
		try
		{
			cui::framework::TreeAccess::SetLogicalParent(
				*raw, logicalParent, &visualOwnershipCommit);
		}
		catch (...)
		{
			const auto error = std::current_exception();
			auto* live = lifetime.Get();
			if (live && !visualOwnershipCommit
				&& !live->GetVisualParent())
				value.reset(live);
			const bool committed = !live || visualOwnershipCommit
				|| live->GetVisualParent()
				|| live->GetLogicalParent() == logicalParent;
			if (!committed || !notificationError)
				std::rethrow_exception(error);
			*notificationError = error;
			return value != nullptr;
		}
		auto* live = lifetime.Get();
		if (!live || visualOwnershipCommit
			|| live->GetVisualParent())
			return false;
		value.reset(live);
		if (live->GetLogicalParent() != logicalParent)
			throw std::logic_error(
				"ContentControl logical parent change did not commit");
		return true;
	}

	template<typename TValue>
	DependencyPropertyOptions<ContentControl, TValue> ContentOptions(
		TValue defaultValue
		CUI_DESIGN_METADATA_ARGUMENTS(
			int order,
			DependencyPropertyPersistence persistence =
				DependencyPropertyPersistence::Metadata))
	{
		DependencyPropertyOptions<ContentControl, TValue> options;
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

const DependencyProperty& ContentControl::ContentProperty()
{
	static const auto registration = []
	{
		auto options = ContentOptions(
			BindingValue{}
			CUI_DESIGN_METADATA_ARGUMENTS(
				10, DependencyPropertyPersistence::Native));
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Browsable = false;
		)
		options.Coerce = [](
			ContentControl& target,
			const BindingValue& proposed) -> std::optional<BindingValue>
		{
			std::wstring error;
			if (!target.ValidateContentCandidate(
				proposed, target._contentTemplate, error))
			{
				target._lastContentError = std::move(error);
				return std::nullopt;
			}
			return proposed;
		};
		options.Changed = [](
			ContentControl& target,
			const BindingValue&, const BindingValue&)
		{
			(void)target.RebuildPresenter();
		};
		return DependencyPropertyRegistry::RegisterStatic<
			ContentControl, BindingValue>(
				DependencyPropertyRegistrationLiteral(L"Content"),
				[](ContentControl& target) { return target.GetContent(); },
				[](ContentControl& target, const BindingValue& value)
				{ target.SetContent(value); }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& ContentControl::ContentTemplateProperty()
{
	static const auto registration = []
	{
		auto options = ContentOptions(
			ItemTemplateReference{}
			CUI_DESIGN_METADATA_ARGUMENTS(
				20, DependencyPropertyPersistence::Native));
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Browsable = false;
		)
		options.Coerce = [](
			ContentControl& target,
			const ItemTemplateReference& proposed)
			-> std::optional<ItemTemplateReference>
		{
			std::wstring error;
			if (!target.ValidateContentCandidate(
				target._content, proposed, error))
			{
				target._lastContentError = std::move(error);
				return std::nullopt;
			}
			return proposed;
		};
		options.Changed = [](
			ContentControl& target,
			const ItemTemplateReference&, const ItemTemplateReference&)
		{
			(void)target.RebuildPresenter();
		};
		return DependencyPropertyRegistry::RegisterStatic<
			ContentControl, ItemTemplateReference>(
				DependencyPropertyRegistrationLiteral(L"ContentTemplate"),
				[](ContentControl& target)
				{ return target.GetContentTemplate(); },
				[](ContentControl& target,
					const ItemTemplateReference& value)
				{ target.SetContentTemplate(value); }, {}, std::move(options));
	}();
	return *registration;
}

ContentControl::ContentControl()
	: Control()
{
}

void ContentControl::OnRender()
{
	if (!IsVisible || !GetPresentationWindow() || !GetDrawingContext()) return;
	// Templated item containers (DataGridCell, ListBoxItem, ...) usually carry a
	// transparent Background whose chrome is drawn by the template root. Such a
	// fill contributes no pixels, so skip the whole recording scaffold for it.
	{
		const auto background = GetComputedBackgroundBrush();
		const bool invisible = background.Kind == cui::drawing::BrushKind::None
			|| (background.Kind == cui::drawing::BrushKind::Solid
				&& (background.Color.a <= 0.0f || background.Opacity <= 0.0f));
		if (invisible && TryCompleteEmptyRender()) return;
	}
	BeginRender();
	const auto size = GetActualSizeDip();
	if (auto* background = CreateBackgroundBrush(
		*GetDrawingContext(), D2D1_SIZE_F{ size.width, size.height }))
	{
		GetDrawingContext()->FillRect(
			0.0f, 0.0f, size.width, size.height, background);
		background->Release();
	}
	EndRender();
}

#if !CUI_ENABLE_DYNAMIC_XAML
void ContentControl::RegisterDependencyProperties()
{
	Control::RegisterDependencyProperties();
}
#endif

cui::core::Size ContentControl::MeasureCore(
	const cui::core::Constraints& available)
{
	return cui::layout::MeasureOverlayChildren(
		GetLayoutChildrenView(), available,
		GetSpecifiedLayout().padding);
}

void ContentControl::Arrange(cui::core::Rect finalRect)
{
	Control::Arrange(finalRect);
	PerformPendingLayout();
}

void ContentControl::RequestLayout()
{
	_contentLayoutPending = true;
	Control::RequestLayout();
}

void ContentControl::OnComputedLayoutSizeChanged()
{
	_contentLayoutPending = true;
}

void ContentControl::PerformPendingLayout()
{
	EnsureVisualContentProjection();
	if (IsLayoutSuspended() || !_contentLayoutPending) return;
	if (GetControlTemplateRoot())
	{
		// Control::Arrange already assigns the complete control slot to the
		// template root. Padding belongs to the authored template (typically via
		// TemplateBinding); consuming it again here double-insets every templated
		// ContentControl and collapses small buttons and icon buttons.  The root
		// must still commit layout invalidated by changes inside the template.
		GetControlTemplateRoot()->UpdateLayout();
		_contentLayoutPending = false;
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
	_contentLayoutPending = false;
}

Control* ContentControl::GetVisualContent() const noexcept
{
	auto* result = FindDirectVisualContent();
	if (result) return result;
	if (auto* presenter = GetTemplateContentPresenter())
		if (auto* content = presenter->GetVisualContent())
			return content;
	return _unpresentedVisualContent.get();
}

Control* ContentControl::FindDirectVisualContent() const noexcept
{
	Control* result = nullptr;
	for (auto* child : GetVisualChildrenView())
	{
		if (!child || IsInfrastructureChild(child)) continue;
		if (result) return nullptr;
		result = child;
	}
	return result;
}

Control* ContentControl::ContentVisualForConfiguration() const noexcept
{
	if (auto* authored = GetVisualContent()) return authored;
	return _presenter;
}

Control* ContentControl::AttachVisualContent(
	std::unique_ptr<Control>& value)
{
	if (!value) return nullptr;
	if (!_content.Empty() || _contentTemplate)
		throw std::logic_error(
			"ContentControl visual content cannot be combined with data content");
	if (GetVisualContent())
		throw std::logic_error(
			"ContentControl accepts at most one visual child");

	auto* raw = value.get();
	const ControlWeakReference lifetime(raw);
	_contentVisualConfigurationPending = true;
	if (auto* templatePresenter = GetTemplateContentPresenter())
	{
		(void)AttachOwnedVisualChildPreserving(
			*templatePresenter, value, this);
	}
	else if (_controlTemplateRoot)
	{
		if (raw->GetVisualParent()
			|| (raw->GetLogicalParent()
				&& raw->GetLogicalParent() != this))
			throw std::logic_error("该控件已属于其他容器");
		if (raw->GetLogicalParent() != this)
		{
			if (!SetLogicalParentPreservingOwnership(value, this))
				throw std::logic_error(
					"ContentControl sidecar content was reparented");
		}
		_unpresentedVisualContent = std::move(value);
	}
	else
	{
		(void)AttachOwnedVisualChildPreserving(*this, value, this);
	}
	RequestLayout();
	InvalidateVisual();
	return raw;
}

Control* ContentControl::SetVisualContent(std::unique_ptr<Control> value)
{
	if (value.get() == ContentControl::GetVisualContent())
		return value.release();
	const ControlWeakReference selfLifetime(this);
	auto previous = ContentControl::DetachVisualContent();
	auto* self = dynamic_cast<ContentControl*>(selfLifetime.Get());
	if (!self) return nullptr;
	if (!value) return nullptr;
	const ControlWeakReference valueLifetime(value.get());
	try
	{
		(void)self->AttachVisualContent(value);
		return valueLifetime.Get();
	}
	catch (...)
	{
		self = dynamic_cast<ContentControl*>(selfLifetime.Get());
		if (self && !self->ContentControl::GetVisualContent() && previous)
			(void)self->AttachVisualContent(previous);
		throw;
	}
}

bool ContentControl::TrySetVisualContent(
	std::unique_ptr<Control>& value) noexcept
{
	if (!value || ContentControl::GetVisualContent()) return false;
	auto* raw = value.get();
	const ControlWeakReference lifetime(raw);
	try
	{
		return AttachVisualContent(value) == raw;
	}
	catch (...)
	{
		// Notification can throw after attachment committed.  In that case the
		// ContentControl owns the child and TrySet succeeded; reporting false
		// would violate its ownership-preserving contract.
		auto* live = lifetime.Get();
		if (live && ContentControl::GetVisualContent() == live)
			return true;
		return false;
	}
}

std::unique_ptr<Control>
ContentControl::DetachVisualContentPreservingLogicalParent()
{
	std::unique_ptr<Control> result;
	if (_unpresentedVisualContent)
		result = std::move(_unpresentedVisualContent);
	else if (auto* content = ContentControl::GetVisualContent())
	{
		if (content->GetVisualParent() == this)
			result = DetachOwnedVisualChildPreserving(*this, content);
		else if (auto* templatePresenter = GetTemplateContentPresenter();
			templatePresenter
			&& content->GetVisualParent() == templatePresenter)
			result = DetachOwnedVisualChildPreserving(
				*templatePresenter, content);
	}
	if (!result) return {};
	RequestLayout();
	InvalidateVisual();
	return result;
}

std::unique_ptr<Control> ContentControl::DetachVisualContent()
{
	auto result = DetachVisualContentPreservingLogicalParent();
	if (!result) return {};
	if (result->GetLogicalParent() == this)
	{
		std::exception_ptr notificationError;
		if (!SetLogicalParentPreservingOwnership(
			result, nullptr, &notificationError))
			return {};
		// The logical-parent notification occurs after the parent field
		// commits. Ownership-returning Detach still returns its unique_ptr.
	}
	return result;
}

bool ContentControl::ValidateVisualChildCollection(
	std::span<Control* const> children,
	std::string& error) const
{
	if (!_changingInfrastructure)
	{
		for (auto* infrastructure : _infrastructureChildren)
		{
			if (std::find(children.begin(), children.end(), infrastructure)
				== children.end())
			{
				error = "ContentControl infrastructure children cannot be mutated directly";
				return false;
			}
		}
	}

	size_t authoredCount = 0;
	for (auto* child : children)
		if (child && !IsInfrastructureChild(child)) ++authoredCount;
	auto* templatePresenter = GetTemplateContentPresenter();
	const bool hasProjectedContent = _unpresentedVisualContent
		|| (templatePresenter && templatePresenter->GetVisualContent());
	if (authoredCount > 1
		|| (authoredCount != 0 && hasProjectedContent))
	{
		error = "ContentControl accepts at most one visual child";
		return false;
	}
	if (authoredCount != 0
		&& (!_content.Empty() || _contentTemplate))
	{
		error = "ContentControl visual content cannot be combined with data content";
		return false;
	}
	return true;
}

void ContentControl::OnVisualChildCollectionChanged(
	const CollectionChangedEventArgs& change,
	std::span<Control* const> previousChildren)
{
	(void)change;
	(void)previousChildren;
	_contentLayoutPending = true;
	// Never detach/reparent while the outer ObservableCollection notification
	// is still unwinding.  Projection is committed by OnApplyTemplate, measure
	// or presentation once collection ownership is stable.
	_contentVisualConfigurationPending = true;
	_visualContentProjectionPending = true;
}

Control* ContentControl::AddInfrastructureChild(
	std::unique_ptr<Control> child,
	InfrastructureChildRole role)
{
	if (!child) return nullptr;
	auto* raw = child.get();
	const ControlWeakReference lifetime(raw);
	_infrastructureChildren.push_back(raw);
	_changingInfrastructure = true;
	try
	{
		if (role == InfrastructureChildRole::TemplateImplementation)
		{
			if (raw->GetTemplatedParent()
				&& raw->GetTemplatedParent() != this)
				throw std::logic_error(
					"template infrastructure already has another template parent");
			if (!raw->GetTemplatedParent())
			{
				auto* released = child.release();
				const ControlWeakReference templatedLifetime(released);
				bool visualOwnershipCommit = false;
				try
				{
					cui::framework::TreeAccess::SetTemplatedParent(
						*released, this, &visualOwnershipCommit);
				}
				catch (...)
				{
					auto* live = templatedLifetime.Get();
					if (live && !visualOwnershipCommit
						&& !live->GetVisualParent()
						&& !live->GetPresentationWindow())
						child.reset(live);
					throw;
				}
				auto* live = templatedLifetime.Get();
				if (!live)
					throw std::logic_error(
						"template infrastructure was destroyed during parent publication");
				if (visualOwnershipCommit || live->GetVisualParent()
					|| live->GetPresentationWindow())
					throw std::logic_error(
						"template infrastructure ownership changed during parent publication");
				child.reset(live);
				if (live->GetTemplatedParent() != this)
					throw std::logic_error(
						"template parent publication did not commit");
			}
			(void)AttachOwnedVisualChildPreserving(
				*this, child, nullptr);
		}
		else
			(void)AttachOwnedVisualChildPreserving(
				*this, child, this);
		auto* live = lifetime.Get();
		const auto expectedLogicalParent =
			role == InfrastructureChildRole::LogicalSlot
			? this : nullptr;
		if (!live || live->GetVisualParent() != this
			|| IndexOfVisualChild(live) < 0
			|| !IsInfrastructureChild(live)
			|| live->GetLogicalParent() != expectedLogicalParent
			|| (role == InfrastructureChildRole::TemplateImplementation
				&& live->GetTemplatedParent() != this))
			throw std::logic_error(
				"ContentControl infrastructure attachment did not commit");
		_changingInfrastructure = false;
	}
	catch (...)
	{
		_changingInfrastructure = false;
		auto* live = lifetime.Get();
		// A notification can throw after the child is already attached. Keep
		// its infrastructure classification so the caller can roll it back
		// without exposing it as authored Content.
		if (!live || live->GetVisualParent() != this)
			_infrastructureChildren.erase(std::remove(
				_infrastructureChildren.begin(),
				_infrastructureChildren.end(), raw),
				_infrastructureChildren.end());
		if ((!live || live->GetVisualParent()) && child)
			(void)child.release();
		throw;
	}
	return raw;
}

std::unique_ptr<Control> ContentControl::DetachInfrastructureChild(
	Control* child,
	std::exception_ptr* notificationError)
{
	if (notificationError) *notificationError = {};
	if (!child || !IsInfrastructureChild(child)) return {};
	const ControlWeakReference lifetime(child);
	_changingInfrastructure = true;
	std::unique_ptr<Control> result;
	try
	{
		result = DetachOwnedVisualChildPreserving(
			*this, child, notificationError);
		_changingInfrastructure = false;
	}
	catch (...)
	{
		_changingInfrastructure = false;
		throw;
	}
	auto* live = lifetime.Get();
	if (!live || (live->GetVisualParent() != this
		&& IndexOfVisualChild(live) < 0))
		_infrastructureChildren.erase(std::remove(
			_infrastructureChildren.begin(),
			_infrastructureChildren.end(), child),
			_infrastructureChildren.end());
	return result;
}

bool ContentControl::IsInfrastructureChild(const Control* child) const noexcept
{
	return child && std::find(
		_infrastructureChildren.begin(),
		_infrastructureChildren.end(), child) != _infrastructureChildren.end();
}

void ContentControl::ConfigureContentVisual(Control& child)
{
	(void)child;
}

void ContentControl::ConfigurePendingContentVisual()
{
	if (!_contentVisualConfigurationPending
		|| _configuringContentVisual) return;
	auto* target = ContentVisualForConfiguration();
	if (!target)
	{
		_contentVisualConfigurationPending = false;
		return;
	}

	const ControlWeakReference lifetime(target);
	_contentVisualConfigurationPending = false;
	_configuringContentVisual = true;
	try
	{
		ConfigureContentVisual(*target);
		_configuringContentVisual = false;
	}
	catch (...)
	{
		_configuringContentVisual = false;
		_contentVisualConfigurationPending = true;
		throw;
	}
	auto* live = lifetime.Get();
	if (!live || ContentVisualForConfiguration() != live)
		_contentVisualConfigurationPending =
			ContentVisualForConfiguration() != nullptr;
}

void ContentControl::ConfigureControlTemplateVisual(Control& child)
{
	Control::ConfigureControlTemplateVisual(child);
}

void ContentControl::PreserveTemplateVisualContent(
	std::exception_ptr* outNotificationError)
{
	if (outNotificationError) *outNotificationError = {};
	auto presenterReference = _templateContentPresenter;
	auto* presenter = dynamic_cast<ContentPresenter*>(
		presenterReference.Get());
	if (!presenter)
	{
		_templateContentPresenter.Reset();
		return;
	}
	if (_controlTemplateRoot && !TemplateRootContains(presenter))
	{
		_templateContentPresenter.Reset();
		return;
	}
	if (_unpresentedVisualContent) return;
	auto* content = presenter->GetVisualContent();
	if (!content) return;
	std::exception_ptr notificationError;
	auto owner = DetachOwnedVisualChildPreserving(
		*presenter, content, &notificationError);
	if (!owner && !notificationError)
		notificationError = std::make_exception_ptr(
			std::logic_error(
				"ContentControl template presenter lost visual content ownership"));
	if (owner && owner->GetLogicalParent() != this)
	{
		std::exception_ptr logicalNotificationError;
		if (!SetLogicalParentPreservingOwnership(
			owner, this, &logicalNotificationError))
		{
			if (!notificationError)
				notificationError = std::make_exception_ptr(
					std::logic_error(
						"ContentControl preserved content was reparented"));
		}
		if (!notificationError)
			notificationError = logicalNotificationError;
	}
	if (owner)
		_unpresentedVisualContent = std::move(owner);
	RequestLayout();
	InvalidateVisual();
	if (notificationError)
	{
		if (outNotificationError)
			*outNotificationError = notificationError;
		else
			std::rethrow_exception(notificationError);
	}
}

void ContentControl::RestoreUnpresentedVisualContent()
{
	if (!_unpresentedVisualContent) return;
	if (!GetTemplateContentPresenter() && _controlTemplateRoot) return;

	auto value = std::move(_unpresentedVisualContent);
	try
	{
		(void)AttachVisualContent(value);
	}
	catch (...)
	{
		if (value) _unpresentedVisualContent = std::move(value);
		throw;
	}
}

bool ContentControl::TemplateRootContains(
	const Control* candidate) const noexcept
{
	if (!_controlTemplateRoot || !candidate) return false;
	std::vector<const Control*> stack{ _controlTemplateRoot };
	while (!stack.empty())
	{
		const auto* current = stack.back();
		stack.pop_back();
		if (current == candidate)
			return dynamic_cast<const ContentPresenter*>(current) != nullptr;
		if (!current) continue;
		for (auto* child : current->GetVisualChildrenView())
			if (child) stack.push_back(child);
	}
	return false;
}

bool ContentControl::CommitPendingTemplateContentPresenter()
{
	auto candidateReference = _pendingTemplateContentPresenter;
	auto* candidate = dynamic_cast<ContentPresenter*>(
		candidateReference.Get());
	if (!candidate)
	{
		ClearPendingTemplateContentPresenter();
		return false;
	}
	if (candidate->GetTemplatedParent() != this)
	{
		ClearPendingTemplateContentPresenter();
		return false;
	}
	if (!TemplateRootContains(candidate)) return false;
	if (GetTemplateContentPresenter() != candidate)
	{
		PreserveTemplateVisualContent();
		// Preserve can publish arbitrary user callbacks. The candidate may have
		// been removed or destroyed while the previous presenter detached its
		// child; never publish the pre-callback raw identity.
		candidate = dynamic_cast<ContentPresenter*>(
			candidateReference.Get());
		if (!candidate || candidate->GetTemplatedParent() != this
			|| !TemplateRootContains(candidate))
		{
			if (!candidate || candidate->GetTemplatedParent() != this)
				ClearPendingTemplateContentPresenter();
			return false;
		}
	}
	_templateContentPresenter = candidate;
	_pendingTemplateContentPresenter.Reset();
	_visualContentProjectionPending = true;
	_contentVisualConfigurationPending = true;
	return true;
}

void ContentControl::ClearPendingTemplateContentPresenter() noexcept
{
	_pendingTemplateContentPresenter.Reset();
}

void ContentControl::EnsureVisualContentProjection()
{
	if (!_visualContentProjectionPending
		&& !_pendingTemplateContentPresenter.HasValue()
		&& !_contentVisualConfigurationPending) return;
	if (_projectingVisualContent)
	{
		_visualContentProjectionPending = true;
		return;
	}
	_projectingVisualContent = true;
	try
	{
		for (size_t pass = 0; pass < 32; ++pass)
		{
			_visualContentProjectionPending = false;
			if (_templateContentPresenter.HasValue()
				&& !GetTemplateContentPresenter())
				_templateContentPresenter.Reset();
			if (auto* active = GetTemplateContentPresenter();
				active && _controlTemplateRoot
				&& !TemplateRootContains(active))
				_templateContentPresenter.Reset();
			(void)CommitPendingTemplateContentPresenter();
			auto* directContent = FindDirectVisualContent();
			if (!_controlTemplateRoot)
			{
				RestoreUnpresentedVisualContent();
			}
			else
			{
				if (directContent)
				{
					std::exception_ptr notificationError;
					auto owner = DetachOwnedVisualChildPreserving(
						*this, directContent, &notificationError);
					if (!owner)
						throw std::logic_error(
							"ContentControl lost direct visual content ownership");
					if (owner->GetLogicalParent() != this)
					{
						std::exception_ptr logicalNotificationError;
						if (!SetLogicalParentPreservingOwnership(
							owner, this, &logicalNotificationError))
							throw std::logic_error(
								"ContentControl unpresented content was reparented");
						if (!notificationError)
							notificationError = logicalNotificationError;
					}
					_unpresentedVisualContent = std::move(owner);
					if (notificationError)
						std::rethrow_exception(notificationError);
				}

				if (GetTemplateContentPresenter())
					RestoreUnpresentedVisualContent();
				// A template with no ContentPresenter intentionally leaves
				// Content logically owned but visually unpresented.
			}
			ConfigurePendingContentVisual();
			if (!_visualContentProjectionPending
				&& !_contentVisualConfigurationPending)
			{
				_projectingVisualContent = false;
				return;
			}
		}
		throw std::logic_error(
			"ContentControl visual content projection did not converge");
	}
	catch (...)
	{
		_projectingVisualContent = false;
		_visualContentProjectionPending = true;
		throw;
	}
}

void ContentControl::OnApplyTemplate()
{
	if (!CommitPendingTemplateContentPresenter())
		ClearPendingTemplateContentPresenter();
	EnsureVisualContentProjection();
	Control::OnApplyTemplate();
}

void ContentControl::PreparePresentation()
{
	EnsureVisualContentProjection();
	Control::PreparePresentation();
}

void ContentControl::PrepareMeasureCore(
	const cui::core::Constraints& available)
{
	(void)available;
	EnsureVisualContentProjection();
}

Control* ContentControl::SetControlTemplateRoot(
	std::unique_ptr<Control> value)
{
	if (value && value.get() == _controlTemplateRoot)
	{
		(void)value.release();
		return _controlTemplateRoot;
	}
	// Validate/configure the replacement while the established tree is still
	// intact. A failing customization must not tear down the old template.
	if (value)
		ConfigureControlTemplateVisualPreservingOwnership(value);
	if (value && _controlTemplateRoot)
	{
		// Template factories always detach before installing a new instance.
		// Reject direct replacement instead of trying to reconstruct presenter,
		// namescope and projection state after a partial swap.
		ClearPendingTemplateContentPresenter();
		throw std::logic_error(
			"ContentControl already owns a ControlTemplate root");
	}
	if (!value)
	{
		(void)DetachVisualChildTemplateRoot();
		return nullptr;
	}

	auto* candidateRoot = value.get();
	const ControlWeakReference candidateLifetime(candidateRoot);
	// Publish the in-flight single-assignment before attachment callbacks can
	// re-enter SetTemplateRoot. Nested installation must observe an occupied
	// slot rather than creating a second hidden infrastructure root.
	_controlTemplateRoot = candidateRoot;
	try
	{
		AddInfrastructureChild(std::move(value));
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		auto* live = candidateLifetime.Get();
		if (live && live->GetVisualParent() == this
			&& IndexOfVisualChild(live) >= 0)
		{
			// Attachment committed before an observer threw. Keep the root
			// published while the regular detach transaction runs; if rollback
			// itself fails, retaining that publication is the only consistent
			// state (a hidden infrastructure child is never acceptable).
			_controlTemplateRoot = live;
			MarkControlTemplateRootAttached();
			try { (void)DetachVisualChildTemplateRoot(); }
			catch (...) {}
		}
		live = candidateLifetime.Get();
		if (live && live->GetVisualParent() == this
			&& IndexOfVisualChild(live) >= 0)
		{
			if (!IsInfrastructureChild(live))
				_infrastructureChildren.push_back(live);
			_controlTemplateRoot = live;
			MarkControlTemplateRootAttached();
		}
		else
		{
			_infrastructureChildren.erase(std::remove(
				_infrastructureChildren.begin(),
				_infrastructureChildren.end(), candidateRoot),
				_infrastructureChildren.end());
			(void)ClearTemplateOwnerSubtree(live, this);
			auto* replacement = _controlTemplateRoot;
			const bool replacementCommitted = replacement
				&& replacement != candidateRoot
				&& replacement->GetVisualParent() == this
				&& IndexOfVisualChild(replacement) >= 0
				&& IsInfrastructureChild(replacement);
			if (replacementCommitted)
				MarkControlTemplateRootAttached();
			else
			{
				if (_controlTemplateRoot == candidateRoot)
					_controlTemplateRoot = nullptr;
				ClearPendingTemplateContentPresenter();
				_templateContentPresenter.Reset();
				MarkControlTemplateRootDetached();
			}
		}
		std::rethrow_exception(originalError);
	}

	auto* liveCandidate = candidateLifetime.Get();
	const bool hostOwnsCandidate = liveCandidate
		&& liveCandidate->GetVisualParent() == this
		&& IndexOfVisualChild(liveCandidate) >= 0;
	const bool candidateInvariant = hostOwnsCandidate
		&& IsInfrastructureChild(liveCandidate)
		&& liveCandidate->GetLogicalParent() == nullptr
		&& liveCandidate->GetTemplatedParent() == this;
	if (!candidateInvariant)
	{
		if (hostOwnsCandidate)
		{
			// Never turn an actually owned visual into an unclassified hidden
			// child merely because a callback disturbed the sidecar vector.
			if (!IsInfrastructureChild(liveCandidate))
				_infrastructureChildren.push_back(liveCandidate);
			_controlTemplateRoot = liveCandidate;
			MarkControlTemplateRootAttached();
			throw std::logic_error(
				"ContentControl template root invariants did not commit");
		}
		_infrastructureChildren.erase(std::remove(
			_infrastructureChildren.begin(),
			_infrastructureChildren.end(), candidateRoot),
			_infrastructureChildren.end());
		if (_controlTemplateRoot == candidateRoot)
			_controlTemplateRoot = nullptr;
		ClearPendingTemplateContentPresenter();
		_templateContentPresenter.Reset();
		(void)ClearTemplateOwnerSubtree(liveCandidate, this);
		try { ClearDeclarativeTemplateScope(); }
		catch (...) {}
		MarkControlTemplateRootDetached();
		throw std::logic_error(
			"ContentControl template root attachment did not commit");
	}

	// Root ownership is now committed. Only from this point may a registered
	// presenter contained by that root become the active projection target.
	_controlTemplateRoot = liveCandidate;
	MarkControlTemplateRootAttached();
	try
	{
		(void)CommitPendingTemplateContentPresenter();
		_visualContentProjectionPending = true;
		EnsureVisualContentProjection();
		OnControlTemplatePresentationChanged();
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		try { (void)DetachVisualChildTemplateRoot(); }
		catch (...) {}
		auto* live = candidateLifetime.Get();
		if (live && (live->GetVisualParent() == this
			|| IndexOfVisualChild(live) >= 0
			|| IsInfrastructureChild(live)))
		{
			_controlTemplateRoot = live;
			MarkControlTemplateRootAttached();
		}
		else if (_controlTemplateRoot == candidateRoot)
		{
			_controlTemplateRoot = nullptr;
			ClearPendingTemplateContentPresenter();
			_templateContentPresenter.Reset();
			MarkControlTemplateRootDetached();
		}
		std::rethrow_exception(originalError);
	}
	RequestLayout();
	InvalidateVisual();
	return _controlTemplateRoot;
}

std::unique_ptr<Control> ContentControl::DetachVisualChildTemplateRoot()
{
	if (!_controlTemplateRoot)
	{
		ClearPendingTemplateContentPresenter();
		_templateContentPresenter = nullptr;
		_visualContentProjectionPending = true;
		EnsureVisualContentProjection();
		MarkControlTemplateRootDetached();
		OnControlTemplatePresentationChanged();
		return {};
	}
	std::exception_ptr preserveNotificationError;
	PreserveTemplateVisualContent(&preserveNotificationError);
	auto* previous = _controlTemplateRoot;
	const ControlWeakReference previousLifetime(previous);
	std::unique_ptr<Control> result;
	std::exception_ptr notificationError = preserveNotificationError;
	try
	{
		std::exception_ptr detachNotificationError;
		result = DetachInfrastructureChild(
			previous, &detachNotificationError);
		if (!notificationError)
			notificationError = detachNotificationError;
	}
	catch (...)
	{
		RestoreUnpresentedVisualContent();
		throw;
	}
	auto* livePrevious = previousLifetime.Get();
	if (livePrevious && (livePrevious->GetVisualParent() == this
		|| IndexOfVisualChild(livePrevious) >= 0
		|| IsInfrastructureChild(livePrevious)))
	{
		_controlTemplateRoot = livePrevious;
		MarkControlTemplateRootAttached();
		RestoreUnpresentedVisualContent();
		if (notificationError)
			std::rethrow_exception(notificationError);
		throw std::logic_error(
			"ContentControl template root rollback did not detach");
	}
	_controlTemplateRoot = nullptr;
	auto cleanupError = result
		? ClearTemplateOwnerSubtreePreservingOwnership(result, this)
		: ClearTemplateOwnerSubtree(livePrevious, this);
	auto* replacement = _controlTemplateRoot;
	const bool replacementCommitted = replacement
		&& replacement->GetVisualParent() == this
		&& IndexOfVisualChild(replacement) >= 0
		&& IsInfrastructureChild(replacement);
	if (replacementCommitted)
	{
		MarkControlTemplateRootAttached();
	}
	else
	{
		_controlTemplateRoot = nullptr;
		try
		{
			ClearDeclarativeTemplateScope();
		}
		catch (...)
		{
			if (!cleanupError) cleanupError = std::current_exception();
		}
		ClearPendingTemplateContentPresenter();
		_templateContentPresenter.Reset();
		_visualContentProjectionPending = true;
		try
		{
			EnsureVisualContentProjection();
		}
		catch (...)
		{
			if (!cleanupError) cleanupError = std::current_exception();
		}
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

bool ContentControl::RegisterTemplateContentPresenter(
	ContentPresenter* presenter)
{
	if (!presenter || presenter->GetTemplatedParent() != this) return false;
	if (GetTemplateContentPresenter() == presenter) return true;
	_pendingTemplateContentPresenter = presenter;
	_visualContentProjectionPending = true;
	RequestLayout();
	return true;
}

void ContentControl::OnControlTemplatePresentationChanged()
{
	if (!_controlTemplateRoot)
	{
		ClearPendingTemplateContentPresenter();
		_templateContentPresenter.Reset();
	}
	else (void)CommitPendingTemplateContentPresenter();
	_visualContentProjectionPending = true;
	EnsureVisualContentProjection();
	(void)RebuildPresenter();
}

void ContentControl::SetContent(BindingValue value)
{
	if (BindingValuesEqual(_content, value)) return;
	_lastContentError.clear();
	(void)SetPropertyField(
		ContentProperty(), _content, std::move(value));
}

std::wstring ContentControl::GetSemanticText() const
{
	std::wstring value;
	return _content.TryGetString(value) ? value : std::wstring{};
}

void ContentControl::SetContentTemplate(ItemTemplateReference value)
{
	if (_contentTemplate == value) return;
	_lastContentError.clear();
	(void)SetPropertyField(
		ContentTemplateProperty(), _contentTemplate, std::move(value));
}

void ContentControl::SetCompiledDisplayMemberPath(
	CompiledBindingPathView value)
{
	if (value.Version != CompiledBindingPathVersion)
		throw std::invalid_argument(
			"ContentControl compiled display path version is unsupported");
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
	if (auto* presenter = GetTemplateContentPresenter())
		presenter->SetCompiledDisplayMemberPath(value);
	(void)RebuildPresenter();
}

void ContentControl::SetContentTypeToken(DataTypeToken value)
{
	if (_contentTypeToken == value) return;
	const auto oldToken = _contentTypeToken;
	_contentTypeToken = value;
	std::wstring validationError;
	if (!ValidateContentCandidate(
		_content, _contentTemplate, validationError))
	{
		_contentTypeToken = oldToken;
		_lastContentError = std::move(validationError);
		return;
	}
	if (RebuildPresenter()) return;
	const auto error = _lastContentError;
	_contentTypeToken = oldToken;
	(void)RebuildPresenter();
	_lastContentError = error;
}

void ContentControl::ApplyContentProjection(
	ContentPresenter& presenter) const
{
	presenter.SetContentTypeToken(_contentTypeToken);
	presenter.SetCompiledDisplayMemberPath(_compiledDisplayMemberPath);
#if CUI_ENABLE_DYNAMIC_XAML
	ApplyAuthoredContentProjection(presenter);
#endif
}

bool ContentControl::ValidateContentCandidate(
	const BindingValue& content,
	const ItemTemplateReference& contentTemplate,
	std::wstring& error) const
{
	error.clear();
	if (GetVisualContent())
	{
		if (!content.Empty() || contentTemplate)
		{
			error = L"ContentControl 的直接视觉内容不能与数据内容同时使用。";
			return false;
		}
		return true;
	}
	// A FrameworkTemplate factory is user code.  Dependency-property coercion
	// can evaluate both the old and candidate effective values, so loading the
	// template here would execute that factory more than once.  The presenter
	// performs the single committed load and reports a construction failure
	// while retaining its previous visual.
	return true;
}

bool ContentControl::RebuildPresenter()
{
	_lastContentError.clear();
	if (GetTemplateContentPresenter() || _controlTemplateRoot)
	{
		if (_presenter)
		{
			auto* previousRaw = _presenter;
			const ControlWeakReference previousLifetime(previousRaw);
			std::exception_ptr notificationError;
			auto previous = DetachInfrastructureChild(
				previousRaw, &notificationError);
			auto* live = previousLifetime.Get();
			if (live && (live->GetVisualParent() == this
				|| IndexOfVisualChild(live) >= 0
				|| IsInfrastructureChild(live)))
				throw std::logic_error(
					"ContentControl generated presenter did not detach");
			_presenter = nullptr;
			if (notificationError)
				std::rethrow_exception(notificationError);
		}
		_contentVisualConfigurationPending = true;
		RequestLayout();
		InvalidateVisual();
		return true;
	}
	if (GetVisualContent())
	{
		if (!_content.Empty() || _contentTemplate)
		{
			_lastContentError =
				L"ContentControl 的直接视觉内容不能与数据内容同时使用。";
			return false;
		}
		return true;
	}

	std::unique_ptr<ContentPresenter> replacement;
	if (!_content.Empty() || _contentTemplate)
	{
		replacement = std::make_unique<ContentPresenter>();
		ApplyContentProjection(*replacement);
		replacement->SetContent(_content);
		replacement->SetContentTemplate(_contentTemplate);
		if (!replacement->LastTemplateError().empty())
		{
			_lastContentError = replacement->LastTemplateError();
			return false;
		}
	}

	std::unique_ptr<Control> previous;
	ControlWeakReference previousLifetime(_presenter);
	std::exception_ptr detachNotificationError;
	if (_presenter)
	{
		auto* previousRaw = _presenter;
		previous = DetachInfrastructureChild(
			previousRaw, &detachNotificationError);
		auto* live = previousLifetime.Get();
		if (live && (live->GetVisualParent() == this
			|| IndexOfVisualChild(live) >= 0
			|| IsInfrastructureChild(live)))
			throw std::logic_error(
				"ContentControl generated presenter did not detach");
		_presenter = nullptr;
	}

	auto* replacementRaw = replacement.get();
	const ControlWeakReference replacementLifetime(replacementRaw);
	try
	{
		if (replacement)
			(void)AddInfrastructureChild(std::move(replacement));
		_presenter = dynamic_cast<ContentPresenter*>(
			replacementLifetime.Get());
	}
	catch (...)
	{
		const auto originalError = std::current_exception();
		auto* liveReplacement = replacementLifetime.Get();
		if (liveReplacement && (liveReplacement->GetVisualParent() == this
			|| IndexOfVisualChild(liveReplacement) >= 0
			|| IsInfrastructureChild(liveReplacement)))
		{
			try
			{
				std::exception_ptr ignoredNotification;
				auto discarded = DetachInfrastructureChild(
					liveReplacement, &ignoredNotification);
			}
			catch (...) {}
		}
		liveReplacement = replacementLifetime.Get();
		if (liveReplacement && (liveReplacement->GetVisualParent() == this
			|| IndexOfVisualChild(liveReplacement) >= 0
			|| IsInfrastructureChild(liveReplacement)))
		{
			_presenter = dynamic_cast<ContentPresenter*>(liveReplacement);
		}
		else if (previous)
		{
			auto* previousRaw = previous.get();
			try
			{
				(void)AddInfrastructureChild(std::move(previous));
			}
			catch (...) {}
			auto* livePrevious = previousLifetime.Get();
			_presenter = livePrevious
				&& livePrevious->GetVisualParent() == this
				&& IsInfrastructureChild(livePrevious)
				? dynamic_cast<ContentPresenter*>(livePrevious)
				: nullptr;
			if (!_presenter && previous && previous.get() == previousRaw)
			{
				// Pre-commit restoration failure leaves the local unique owner
				// intact; it will be destroyed without a hidden tree child.
			}
		}
		else _presenter = nullptr;
		std::rethrow_exception(originalError);
	}
	_contentVisualConfigurationPending = true;
	ConfigurePendingContentVisual();
	RequestLayout();
	InvalidateVisual();
	if (detachNotificationError)
		std::rethrow_exception(detachNotificationError);
	return true;
}
