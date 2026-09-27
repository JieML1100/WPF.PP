#include "TabControl.h"

#include "InputManager.h"
#include "StyleInfrastructure.h"
#include "TemplateInfrastructure.h"
#include "TreeInfrastructure.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{
	class TabItemsHost final : public Panel
	{
	public:
		explicit TabItemsHost(TabControl& owner)
			: _owner(&owner)
		{
		}

		cui::core::Size MeasureCore(
			const cui::core::Constraints& available) override
		{
			const auto maximum = available.Normalized().maximum;
			const auto padding = GetSpecifiedLayout().padding;
			const bool vertical = IsVertical();
			const cui::core::Size childMaximum{
				vertical
					? (std::max)(0.0f,
						maximum.width - padding.Horizontal())
					: cui::core::Infinity,
				vertical
					? cui::core::Infinity
					: (std::max)(0.0f,
						maximum.height - padding.Vertical()) };
			float primary = 0.0f;
			float cross = 0.0f;
			for (auto* child : GetLayoutChildrenView())
			{
				if (!child || child->IsCollapsed()) continue;
				const auto desired = child->Measure(
					cui::core::Constraints{ childMaximum });
				const auto margin = child->Margin;
				if (vertical)
				{
					primary += desired.height + margin.Top + margin.Bottom;
					cross = (std::max)(cross,
						desired.width + margin.Left + margin.Right);
				}
				else
				{
					primary += desired.width + margin.Left + margin.Right;
					cross = (std::max)(cross,
						desired.height + margin.Top + margin.Bottom);
				}
			}
			return vertical
				? cui::core::Size{
					cross + padding.Horizontal(),
					primary + padding.Vertical() }
				: cui::core::Size{
					primary + padding.Horizontal(),
					cross + padding.Vertical() };
		}

	protected:
		void RequestLayout() override
		{
			_layoutPending = true;
			Panel::RequestLayout();
		}

		void OnComputedLayoutSizeChanged() override
		{
			_layoutPending = true;
		}

		void PerformPendingLayout() override
		{
			if (IsLayoutSuspended() || !_layoutPending) return;
			const auto size = GetActualSizeDip().NonNegative();
			const auto padding = GetSpecifiedLayout().padding;
			const bool vertical = IsVertical();
			const float availablePrimary = vertical
				? (std::max)(0.0f, size.height - padding.Vertical())
				: (std::max)(0.0f, size.width - padding.Horizontal());
			const float availableCross = vertical
				? (std::max)(0.0f, size.width - padding.Horizontal())
				: (std::max)(0.0f, size.height - padding.Vertical());

			struct Slot final
			{
				Control* Child = nullptr;
				Thickness Margin{};
				float Primary = 0.0f;
			};
			std::vector<Slot> slots;
			float desiredPrimary = 0.0f;
			for (auto* child : GetLayoutChildrenView())
			{
				if (!child || child->IsCollapsed()) continue;
				const auto margin = child->Margin;
				const auto desired = child->Measure(cui::core::Constraints{
					vertical
						? cui::core::Size{ availableCross, cui::core::Infinity }
						: cui::core::Size{ cui::core::Infinity, availableCross } });
				const float primary = vertical
					? desired.height + margin.Top + margin.Bottom
					: desired.width + margin.Left + margin.Right;
				slots.push_back(Slot{ child, margin, primary });
				desiredPrimary += primary;
			}
			const float scale = desiredPrimary > availablePrimary
				&& desiredPrimary > 0.0f
				? availablePrimary / desiredPrimary : 1.0f;
			float cursor = vertical ? padding.top : padding.left;
			for (const auto& slot : slots)
			{
				const float slotPrimary = slot.Primary * scale;
				if (vertical)
				{
					const float marginBefore = slot.Margin.Top * scale;
					const float marginAfter = slot.Margin.Bottom * scale;
					slot.Child->Arrange(cui::core::Rect{
						padding.left + slot.Margin.Left,
						cursor + marginBefore,
						(std::max)(0.0f, availableCross
							- slot.Margin.Left - slot.Margin.Right),
						(std::max)(0.0f, slotPrimary
							- marginBefore - marginAfter) });
				}
				else
				{
					const float marginBefore = slot.Margin.Left * scale;
					const float marginAfter = slot.Margin.Right * scale;
					slot.Child->Arrange(cui::core::Rect{
						cursor + marginBefore,
						padding.top + slot.Margin.Top,
						(std::max)(0.0f, slotPrimary
							- marginBefore - marginAfter),
						(std::max)(0.0f, availableCross
							- slot.Margin.Top - slot.Margin.Bottom) });
				}
				cursor += slotPrimary;
			}
			_layoutPending = false;
		}

		bool ValidateVisualChildCollection(
			std::span<Control* const> children,
			std::string& error) const override
		{
			for (auto* child : children)
			{
				if (dynamic_cast<TabItem*>(child)) continue;
				error = "TabControl ItemsHost accepts TabItem children only";
				return false;
			}
			return true;
		}

	private:
		bool IsVertical() const noexcept
		{
			auto* owner = dynamic_cast<TabControl*>(_owner.Get());
			return owner && (owner->TabStripPlacement == Dock::Left
				|| owner->TabStripPlacement == Dock::Right);
		}

		ControlWeakReference _owner;
		bool _layoutPending = true;
	};

	template<typename TOwner>
	auto PropertySubscriber(
		const DependencyProperty& (*propertyAccessor)())
	{
		return [propertyAccessor](
			TOwner& target,
			DependencyPropertyMetadata::ChangeHandler handler,
			DataSourceUpdateMode)
		{
			return target.OnPropertyValueChanged.Subscribe(
				[propertyAccessor, handler = std::move(handler)](
					DependencyObject*,
					const DependencyPropertyChangedEventArgs& args)
				{
					if (args.Property == &propertyAccessor()) handler();
				});
		};
	}

	bool PointInRect(
		const D2D1_RECT_F& rect, float x, float y) noexcept
	{
		return x >= rect.left && x < rect.right
			&& y >= rect.top && y < rect.bottom;
	}

	float RectWidth(const D2D1_RECT_F& rect) noexcept
	{
		return (std::max)(0.0f, rect.right - rect.left);
	}

	float RectHeight(const D2D1_RECT_F& rect) noexcept
	{
		return (std::max)(0.0f, rect.bottom - rect.top);
	}

	bool IsVerticalStrip(Dock placement) noexcept
	{
		return placement == Dock::Left || placement == Dock::Right;
	}

}

const DependencyProperty& TabItem::IsSelectedProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<TabItem, bool> options;
		options.DefaultValue = false;
		options.Flags = DependencyPropertyFlags::AffectsRender;
		options.IsReadOnly = false;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"State";
		options.Design.CategoryOrder = 70;
		options.Design.Order = 10;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		options.Design.Browsable = true;
		)
		return DependencyPropertyRegistry::RegisterStatic<TabItem, bool>(
			DependencyPropertyRegistrationLiteral(L"IsSelected"),
			[](TabItem& target) { return target.IsSelected; },
			[](TabItem& target, const bool& value)
			{ target.ApplyIsSelectedValue(value); },
			PropertySubscriber<TabItem>(&TabItem::IsSelectedProperty),
			std::move(options));
	}();
	return *registration;
}

const DependencyProperty& TabItem::TabStripPlacementProperty()
{
	return TabStripPlacementPropertyKey().Property();
}

const DependencyProperty& TabControl::TabStripPlacementProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<TabControl, Dock> options;
		options.DefaultValue = Dock::Top;
		options.Flags = DependencyPropertyFlags::AffectsMeasure
			| DependencyPropertyFlags::AffectsArrange
			| DependencyPropertyFlags::AffectsRender;
		options.Validate = [](const Dock& value)
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
		options.Changed = [](
			TabControl& target, const Dock&, const Dock&)
		{
			target.RequestLayout();
			target.InvalidateVisual();
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Layout";
		options.Design.CategoryOrder = 100;
		options.Design.Order = 10;
		options.Design.Editor = DependencyPropertyEditorKind::Choice;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		options.Design.Choices = {
			{ L"Left", BindingValue(Dock::Left) },
			{ L"Top", BindingValue(Dock::Top) },
			{ L"Right", BindingValue(Dock::Right) },
			{ L"Bottom", BindingValue(Dock::Bottom) }
		};
		)
		return DependencyPropertyRegistry::RegisterStatic<TabControl, Dock>(
			DependencyPropertyRegistrationLiteral(L"TabStripPlacement"),
			[](TabControl& target) { return target.TabStripPlacement; },
			[](TabControl& target, const Dock& value)
			{ target.TabStripPlacement = value; },
			PropertySubscriber<TabControl>(
				&TabControl::TabStripPlacementProperty),
			std::move(options));
	}();
	return *registration;
}

const DependencyProperty& TabControl::ContentTemplateProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<TabControl, ItemTemplateReference> options;
		options.DefaultValue = ItemTemplateReference{};
		options.Flags = DependencyPropertyFlags::AffectsMeasure
			| DependencyPropertyFlags::AffectsArrange;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Content";
		options.Design.CategoryOrder = 60;
		options.Design.Order = 10;
		options.Design.Editor = DependencyPropertyEditorKind::Auto;
		options.Design.Persistence = DependencyPropertyPersistence::Metadata;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			TabControl, ItemTemplateReference>(
				DependencyPropertyRegistrationLiteral(L"ContentTemplate"),
				[](TabControl& target) { return target.GetContentTemplate(); },
				[](TabControl& target, const ItemTemplateReference& value)
				{ target.SetContentTemplate(value); },
				PropertySubscriber<TabControl>(
					&TabControl::ContentTemplateProperty),
				std::move(options));
	}();
	return *registration;
}

const DependencyProperty& TabControl::SelectedContentProperty()
{
	return SelectedContentPropertyKey().Property();
}

const DependencyProperty& TabControl::SelectedContentTemplateProperty()
{
	return SelectedContentTemplatePropertyKey().Property();
}

const DependencyPropertyKey& TabItem::TabStripPlacementPropertyKey()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<TabItem, Dock> options;
		options.DefaultValue = Dock::Top;
		options.Flags = DependencyPropertyFlags::AffectsMeasure
			| DependencyPropertyFlags::AffectsArrange
			| DependencyPropertyFlags::AffectsRender;
		options.IsReadOnly = true;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Layout";
		options.Design.CategoryOrder = 100;
		options.Design.Order = 10;
		options.Design.Editor = DependencyPropertyEditorKind::Choice;
		options.Design.Browsable = false;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		options.Design.Choices = {
			{ L"Left", BindingValue(Dock::Left) },
			{ L"Top", BindingValue(Dock::Top) },
			{ L"Right", BindingValue(Dock::Right) },
			{ L"Bottom", BindingValue(Dock::Bottom) }
		};
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<TabItem, Dock>(
			DependencyPropertyRegistrationLiteral(L"TabStripPlacement"),
			[](TabItem& target) { return target.TabStripPlacement; },
			[](TabItem& target, const Dock& value)
			{
				(void)target.SetReadOnlyPropertyField(
					TabStripPlacementPropertyKey(),
					target._tabStripPlacement, value);
			},
			PropertySubscriber<TabItem>(
				&TabItem::TabStripPlacementProperty),
			std::move(options));
	}();
	return registration.Key();
}

const DependencyPropertyKey& TabControl::SelectedContentPropertyKey()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<TabControl, BindingValue> options;
		options.DefaultValue = BindingValue{};
		options.IsReadOnly = true;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Content";
		options.Design.CategoryOrder = 60;
		options.Design.Order = 20;
		options.Design.Browsable = false;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<
			TabControl, BindingValue>(
				DependencyPropertyRegistrationLiteral(L"SelectedContent"),
				[](TabControl& target) { return target.GetSelectedContent(); },
				[](TabControl& target, const BindingValue& value)
				{
					(void)target.SetReadOnlyPropertyField(
						SelectedContentPropertyKey(),
						target._selectedContent, value);
				},
				PropertySubscriber<TabControl>(
					&TabControl::SelectedContentProperty),
				std::move(options));
	}();
	return registration.Key();
}

const DependencyPropertyKey& TabControl::SelectedContentTemplatePropertyKey()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<TabControl, ItemTemplateReference> options;
		options.DefaultValue = ItemTemplateReference{};
		options.IsReadOnly = true;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Content";
		options.Design.CategoryOrder = 60;
		options.Design.Order = 30;
		options.Design.Browsable = false;
		options.Design.Persistence = DependencyPropertyPersistence::Transient;
		)
		return DependencyPropertyRegistry::RegisterReadOnlyStatic<
			TabControl, ItemTemplateReference>(
				DependencyPropertyRegistrationLiteral(
					L"SelectedContentTemplate"),
				[](TabControl& target)
				{ return target.GetSelectedContentTemplate(); },
				[](TabControl& target, const ItemTemplateReference& value)
				{
					(void)target.SetReadOnlyPropertyField(
						SelectedContentTemplatePropertyKey(),
						target._selectedContentTemplate, value);
				},
				PropertySubscriber<TabControl>(
					&TabControl::SelectedContentTemplateProperty),
				std::move(options));
	}();
	return registration.Key();
}

TabItem::TabItem()
{
	RegisterDependencyProperties();
	EnsureClassHandlers();
}

Control* TabItem::GetVisualContent() const noexcept
{
	auto* owner = dynamic_cast<TabControl*>(GetLogicalParent());
	if (owner && owner->_projectedVisualItem.Get() == this)
		if (auto* projected = owner->_projectedVisualContent.Get())
			return projected;
	return ContentControl::GetVisualContent();
}

Control* TabItem::SetVisualContent(std::unique_ptr<Control> value)
{
	const ControlWeakReference selfLifetime(this);
	auto* candidate = value.get();
	const ControlWeakReference candidateLifetime(candidate);
	auto* owner = dynamic_cast<TabControl*>(GetLogicalParent());
	if (owner && owner->_projectedVisualItem.Get() == this)
	{
		owner->RestoreProjectedVisualContent();
		if (!selfLifetime.Get()) return nullptr;
	}
	auto* self = dynamic_cast<TabItem*>(selfLifetime.Get());
	if (!self) return nullptr;
	auto* result = self->ContentControl::SetVisualContent(std::move(value));
	const bool candidateCommitted = result == candidate;
	self = dynamic_cast<TabItem*>(selfLifetime.Get());
	owner = self
		? dynamic_cast<TabControl*>(self->GetLogicalParent()) : nullptr;
	if (owner)
	{
		const ControlWeakReference ownerLifetime(owner);
		owner->_contentProjectionPending = true;
		owner->SynchronizeSelectionProjection();
		if (auto* liveOwner = dynamic_cast<TabControl*>(ownerLifetime.Get()))
			liveOwner->RequestLayout();
	}
	return candidateCommitted ? candidateLifetime.Get() : nullptr;
}

bool TabItem::TrySetVisualContent(
	std::unique_ptr<Control>& value) noexcept
{
	if (GetVisualContent()) return false;
	const ControlWeakReference selfLifetime(this);
	const bool attached = ContentControl::TrySetVisualContent(value);
	if (attached)
		if (auto* self = dynamic_cast<TabItem*>(selfLifetime.Get()))
			if (auto* owner = dynamic_cast<TabControl*>(
				self->GetLogicalParent()))
		{
			const ControlWeakReference ownerLifetime(owner);
			owner->_contentProjectionPending = true;
			try
			{
				owner->SynchronizeSelectionProjection();
				if (auto* liveOwner = dynamic_cast<TabControl*>(
					ownerLifetime.Get()))
					liveOwner->RequestLayout();
			}
			catch (...) {}
		}
	return attached;
}

std::unique_ptr<Control> TabItem::DetachVisualContent()
{
	const ControlWeakReference selfLifetime(this);
	auto* owner = dynamic_cast<TabControl*>(GetLogicalParent());
	if (owner && owner->_projectedVisualItem.Get() == this)
	{
		owner->RestoreProjectedVisualContent();
		if (!selfLifetime.Get()) return {};
	}
	auto* self = dynamic_cast<TabItem*>(selfLifetime.Get());
	if (!self) return {};
	const ControlWeakReference contentLifetime(
		self->ContentControl::GetVisualContent());
	auto result = self->ContentControl::DetachVisualContent();
	auto* liveContent = contentLifetime.Get();
	self = dynamic_cast<TabItem*>(selfLifetime.Get());
	if (liveContent && (result.get() == liveContent || !self
		|| self->ContentControl::GetVisualContent() != liveContent))
	{
		try
		{
			cui::framework::TemplateAccess::SetPresentationSuppressed(
				*liveContent, false);
		}
		catch (...)
		{
			// Ownership-returning detach has already committed. Presentation
			// cleanup notifications must not destroy the caller's return token.
		}
	}
	self = dynamic_cast<TabItem*>(selfLifetime.Get());
	owner = self
		? dynamic_cast<TabControl*>(self->GetLogicalParent()) : nullptr;
	if (owner)
	{
		const ControlWeakReference ownerLifetime(owner);
		owner->_contentProjectionPending = true;
		try
		{
			owner->SynchronizeSelectionProjection();
			if (auto* liveOwner = dynamic_cast<TabControl*>(
				ownerLifetime.Get()))
				liveOwner->RequestLayout();
		}
		catch (...)
		{
			// The detached unique_ptr remains the primary result. A later layout
			// pass will retry the projection that was left pending.
		}
	}
	return result;
}

void TabItem::RegisterDependencyProperties()
{
	HeaderedContentControl::RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	(void)IsSelectedProperty();
	(void)TabStripPlacementProperty();
#endif
}

GET_CPP(TabItem, bool, IsSelected)
{
	return _isSelected;
}

SET_CPP(TabItem, bool, IsSelected)
{
	if (!SetPropertyField(IsSelectedProperty(), _isSelected, value)) return;
	auto* owner = dynamic_cast<TabControl*>(GetLogicalParent());
	if (!owner) return;
	const int index = owner->IndexOfItem(this);
	if (value) (void)owner->SelectItem(index);
	else if (owner->GetSelectedIndex() == index)
		(void)owner->SelectIndex(-1);
}

void TabItem::SetCurrentIsSelected(bool value)
{
	(void)SetCurrentPropertyField(IsSelectedProperty(), _isSelected, value);
}

void TabItem::ApplyIsSelectedValue(bool value)
{
	if (_isSelected == value) return;
	if (!SetPropertyField(IsSelectedProperty(), _isSelected, value)) return;
	SetStyleState(ControlStyleState::Selected, value);
	RoutedEventArgs args;
	if (value) Selected(this, args);
	else Unselected(this, args);
	InvalidateVisual();
}

GET_CPP(TabItem, Dock, TabStripPlacement)
{
	return _tabStripPlacement;
}

void TabItem::SetTabStripPlacementProjection(Dock value)
{
	(void)SetReadOnlyPropertyField(
		TabStripPlacementPropertyKey(),
		_tabStripPlacement,
		value);
}

void TabItem::EnsureClassHandlers()
{
	static const std::vector<EventConnection> handlers = []
	{
		std::vector<EventConnection> result;
		result.push_back(RoutedEventManager::RegisterClassHandler(
			UIClass::UI_TabItem,
			RoutedEventId::MouseDown,
			&TabItem::HandleDescendantPointerPress));
		result.push_back(RoutedEventManager::RegisterClassHandler(
			UIClass::UI_TabItem,
			RoutedEventId::MouseDoubleClick,
			&TabItem::HandleDescendantPointerPress));
		return result;
	}();
	(void)handlers;
}

bool TabItem::IsOriginalSourceWithinHeader(Control* source) const noexcept
{
	auto* header = const_cast<TabItem*>(this)
		->FindDeclarativeTemplatePart(
			MakeTemplatePartToken(L"PART_Header"));
	if (!header || !source) return false;
	for (auto* current = source; current; current = current->GetRoutedParent())
	{
		if (current == header) return true;
		if (current == this) break;
	}
	return false;
}

void TabItem::HandleDescendantPointerPress(
	Control* sender,
	RoutedEventArgs& args)
{
	auto* item = dynamic_cast<TabItem*>(sender);
	if (!item || args.Handled || !item->IsEffectivelyEnabled()
		|| !item->IsVisible) return;
	auto& mouse = static_cast<MouseEventArgs&>(args);
	if (mouse.ChangedButton != MouseButton::Left
		|| !item->IsOriginalSourceWithinHeader(args.OriginalSource)) return;
	auto* owner = dynamic_cast<TabControl*>(item->GetLogicalParent());
	if (!owner) return;
	const int index = owner->IndexOfItem(item);
	if (index >= 0 && owner->FocusAndSelectItem(index))
		args.Handled = true;
}

bool TabItem::HandlesNavigationKey(Key key) const
{
	auto* owner = dynamic_cast<TabControl*>(GetLogicalParent());
	return owner
		? owner->HandlesNavigationKey(key)
		: HeaderedContentControl::HandlesNavigationKey(key);
}

bool TabItem::ProcessInput(const InputReport& input)
{
	if (input.Kind == InputReportKind::PointerDown
		&& input.ChangedButton == MouseButton::Left)
	{
		auto* owner = dynamic_cast<TabControl*>(GetLogicalParent());
		if (owner && owner->FocusAndSelectItem(owner->IndexOfItem(this)))
			return true;
	}
	if (input.Kind == InputReportKind::KeyDown)
	{
		auto* owner = dynamic_cast<TabControl*>(GetLogicalParent());
		if ((input.Key == Key::Space || input.Key == Key::Return)
			&& owner)
			return owner->FocusAndSelectItem(owner->IndexOfItem(this));
		if (owner && owner->ProcessTabNavigationKey(input)) return true;
	}
	return HeaderedContentControl::ProcessInput(input);
}

void TabControl::RegisterDependencyProperties()
{
	Selector::RegisterDependencyProperties();
	TabItem::RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	(void)TabStripPlacementProperty();
	(void)ContentTemplateProperty();
	(void)SelectedContentProperty();
	(void)SelectedContentTemplateProperty();
#endif
}

GET_CPP(TabControl, Dock, TabStripPlacement)
{
	return _tabStripPlacement;
}

SET_CPP(TabControl, Dock, TabStripPlacement)
{
	if (!SetPropertyField(
		TabStripPlacementProperty(), _tabStripPlacement, value)) return;
	for (int index = 0; index < static_cast<int>(ItemCount()); ++index)
		if (auto* item = GetItem(index))
			item->SetTabStripPlacementProjection(value);
	RequestLayout();
}

void TabControl::SetContentTemplate(ItemTemplateReference value)
{
	const ControlWeakReference ownerLifetime(this);
	if (!SetPropertyField(
		ContentTemplateProperty(), _contentTemplate, std::move(value))) return;
	if (ownerLifetime.Get() != this) return;
	_selectionProjectionPending = true;
	_contentProjectionPending = true;
	SynchronizeSelectionProjection();
	if (ownerLifetime.Get() != this) return;
	SynchronizeSelectedContentHost();
	if (ownerLifetime.Get() != this) return;
	RequestLayout();
}

TabControl::TabControl()
	: Selector()
{
	RegisterDependencyProperties();
	EnsureClassHandlers();
	ReplaceItemsHostCore(std::make_unique<TabItemsHost>(*this));
}

TabControl::~TabControl()
{
	_selectedContentProjectionObservation.Disconnect();
	_observedSelectedContentItem.Reset();
	try { RestoreProjectedVisualContent(); }
	catch (...) {}
}

void TabControl::EnsureClassHandlers()
{
	static const std::vector<EventConnection> handlers = []
	{
		std::vector<EventConnection> result;
		result.push_back(RoutedEventManager::RegisterClassHandler(
			UIClass::UI_TabControl,
			RoutedEventId::MouseDown,
			&TabControl::HandleRoutedPointerPress));
		result.push_back(RoutedEventManager::RegisterClassHandler(
			UIClass::UI_TabControl,
			RoutedEventId::MouseDoubleClick,
			&TabControl::HandleRoutedPointerPress));
		return result;
	}();
	(void)handlers;
}

void TabControl::HandleRoutedPointerPress(
	Control* sender,
	RoutedEventArgs& args)
{
	auto* owner = dynamic_cast<TabControl*>(sender);
	if (!owner || args.Handled || !owner->IsEffectivelyEnabled()
		|| !owner->IsVisible) return;
	auto& mouse = static_cast<MouseEventArgs&>(args);
	if (mouse.ChangedButton != MouseButton::Left) return;
	int index = -1;
	if (owner->TryGetTabHeaderIndexAt(mouse.X, mouse.Y, index)
		&& owner->FocusAndSelectItem(index))
		args.Handled = true;
}

D2D1_RECT_F TabControl::GetTabStripRect() const noexcept
{
	if (auto* presenter = const_cast<TabControl*>(this)
		->FindDeclarativeTemplatePart(
			MakeTemplatePartToken(L"PART_ItemsPresenter")))
	{
		const auto actual = presenter->GetActualSizeDip();
		if (actual.width > 0.0f && actual.height > 0.0f)
		{
			const auto ownerOrigin = GetAbsoluteLocationDip();
			const auto bounds = presenter->GetAbsoluteRectDip();
			return D2D1::RectF(
				bounds.Left() - ownerOrigin.x,
				bounds.Top() - ownerOrigin.y,
				bounds.Right() - ownerOrigin.x,
				bounds.Bottom() - ownerOrigin.y);
		}
	}
	const auto size = GetActualSizeDip().NonNegative();
	const float width = size.width;
	const float height = size.height;
	const bool hasItems = ItemCount() != 0;
	const float horizontal = (std::min)(
		hasItems ? DefaultHeaderExtent : 0.0f,
		height);
	const float vertical = (std::min)(
		hasItems ? DefaultVerticalStripExtent : 0.0f,
		width);
	switch (_tabStripPlacement)
	{
	case Dock::Bottom:
		return D2D1::RectF(0.0f, height - horizontal, width, height);
	case Dock::Left:
		return D2D1::RectF(0.0f, 0.0f, vertical, height);
	case Dock::Right:
		return D2D1::RectF(width - vertical, 0.0f, width, height);
	case Dock::Top:
	default:
		return D2D1::RectF(0.0f, 0.0f, width, horizontal);
	}
}

D2D1_RECT_F TabControl::GetContentRect() const noexcept
{
	if (auto* chrome = const_cast<TabControl*>(this)
		->FindDeclarativeTemplatePart(
			MakeTemplatePartToken(L"PART_ContentChrome")))
	{
		const auto actual = chrome->GetActualSizeDip();
		if (actual.width > 0.0f && actual.height > 0.0f)
		{
			const auto ownerOrigin = GetAbsoluteLocationDip();
			const auto bounds = chrome->GetAbsoluteRectDip();
			return D2D1::RectF(
				bounds.Left() - ownerOrigin.x,
				bounds.Top() - ownerOrigin.y,
				bounds.Right() - ownerOrigin.x,
				bounds.Bottom() - ownerOrigin.y);
		}
	}
	const auto size = GetActualSizeDip().NonNegative();
	const auto strip = GetTabStripRect();
	switch (_tabStripPlacement)
	{
	case Dock::Bottom:
		return D2D1::RectF(0.0f, 0.0f, size.width, strip.top);
	case Dock::Left:
		return D2D1::RectF(strip.right, 0.0f, size.width, size.height);
	case Dock::Right:
		return D2D1::RectF(0.0f, 0.0f, strip.left, size.height);
	case Dock::Top:
	default:
		return D2D1::RectF(0.0f, strip.bottom, size.width, size.height);
	}
}

D2D1_RECT_F TabControl::GetTabHeaderRect(int index) const noexcept
{
	const int count = static_cast<int>(ItemCount());
	if (index < 0 || index >= count || count <= 0) return {};
	if (auto* item = GetItem(index))
	{
		const auto actual = item->GetActualSizeDip();
		if (actual.width > 0.0f && actual.height > 0.0f)
		{
			const auto ownerOrigin = GetAbsoluteLocationDip();
			const auto bounds = item->GetAbsoluteRectDip();
			return D2D1::RectF(
				bounds.Left() - ownerOrigin.x,
				bounds.Top() - ownerOrigin.y,
				bounds.Right() - ownerOrigin.x,
				bounds.Bottom() - ownerOrigin.y);
		}
	}
	const auto strip = GetTabStripRect();
	const bool vertical = IsVerticalStrip(_tabStripPlacement);
	const float available = vertical
		? RectHeight(strip)
		: RectWidth(strip);
	const float extent = count > 0 ? available / count : 0.0f;
	const float start = extent * index;
	if (vertical)
	{
		return D2D1::RectF(
			strip.left,
			strip.top + start,
			strip.right,
			strip.top + start + extent);
	}
	return D2D1::RectF(
		strip.left + start,
		strip.top,
		strip.left + start + extent,
		strip.bottom);
}

bool TabControl::TryGetTabHeaderIndexAt(
	int localX, int localY, int& outIndex) const noexcept
{
	outIndex = -1;
	const auto strip = GetTabStripRect();
	if (!PointInRect(strip,
		static_cast<float>(localX), static_cast<float>(localY))) return false;
	for (int index = 0; index < static_cast<int>(ItemCount()); ++index)
	{
		if (!PointInRect(GetTabHeaderRect(index),
			static_cast<float>(localX), static_cast<float>(localY))) continue;
		outIndex = index;
		return true;
	}
	return false;
}

bool TabControl::ShouldHitTestChildrenAt(int localX, int localY) const
{
	int headerIndex = -1;
	if (TryGetTabHeaderIndexAt(localX, localY, headerIndex))
		return true;
	return PointInRect(
		GetContentRect(),
		static_cast<float>(localX),
		static_cast<float>(localY));
}

bool TabControl::HandlesNavigationKey(Key key) const
{
	const bool vertical = IsVerticalStrip(_tabStripPlacement);
	switch (key)
	{
	case Key::Left:
	case Key::Right:
		return !vertical;
	case Key::Up:
	case Key::Down:
		return vertical;
	case Key::Home:
	case Key::End:
		return true;
	default:
		return false;
	}
}

void TabControl::PreparePresentation()
{
	SynchronizeSelectionProjection();
	SynchronizeSelectedContentHost();
	Selector::PreparePresentation();
}

void TabControl::PerformPendingLayout()
{
	if (IsLayoutSuspended() || !IsItemsLayoutPending()) return;
	SynchronizeSelectionProjection();
	SynchronizeSelectedContentHost();
	ItemsControl::PerformPendingLayout();
}

void TabControl::RefreshSelectedContentProjection()
{
	const ControlWeakReference ownerLifetime(this);
	const int selectedIndex = SelectedIndex;
	auto* selectedItem = GetItem(selectedIndex);
	const ControlWeakReference selectedLifetime(selectedItem);
	BindingValue content;
	ItemTemplateReference contentTemplate;
	DataTypeToken contentType;
	if (selectedItem)
	{
		const bool hasVisual = HasProjectedVisualContent(selectedItem)
			|| selectedItem->ContentControl::GetVisualContent() != nullptr;
		if (!hasVisual)
		{
			content = selectedItem->GetContent();
			contentTemplate = selectedItem->GetContentTemplate()
				? selectedItem->GetContentTemplate() : _contentTemplate;
			contentType = selectedItem->GetContentTypeToken();
		}
	}
	auto selectedPairIsCurrent = [&]()
	{
		auto* currentItem = dynamic_cast<TabItem*>(selectedLifetime.Get());
		if (SelectedIndex != selectedIndex
			|| GetItem(selectedIndex) != currentItem
			|| currentItem != selectedItem) return false;
		BindingValue currentContent;
		ItemTemplateReference currentTemplate;
		DataTypeToken currentType;
		if (currentItem)
		{
			const bool hasVisual = HasProjectedVisualContent(currentItem)
				|| currentItem->ContentControl::GetVisualContent() != nullptr;
			if (!hasVisual)
			{
				currentContent = currentItem->GetContent();
				currentTemplate = currentItem->GetContentTemplate()
					? currentItem->GetContentTemplate() : _contentTemplate;
				currentType = currentItem->GetContentTypeToken();
			}
		}
		const bool sameContent = BindingItemValuesEqual(
			content, currentContent);
		const bool sameTemplate = contentTemplate == currentTemplate;
		const bool sameType = contentType == currentType;
		return sameContent && sameTemplate && sameType;
	};
	// The central ContentPresenter is vacated before this pair is published.
	// Publish the template first so the subsequent Content notification sees
	// one complete new pair rather than trying to materialize new data through
	// the previous page's template.
	(void)SetReadOnlyPropertyField(
		SelectedContentTemplatePropertyKey(),
		_selectedContentTemplate,
		contentTemplate);
	if (ownerLifetime.Get() != this) return;
	const bool templatePairCurrent = selectedPairIsCurrent();
	if (_selectionProjectionPending
		|| !templatePairCurrent)
	{
		_selectionProjectionPending = true;
		_contentProjectionPending = true;
		return;
	}
	(void)SetReadOnlyPropertyField(
		SelectedContentPropertyKey(),
		_selectedContent,
		content);
	if (ownerLifetime.Get() != this) return;
	const bool contentPairCurrent = selectedPairIsCurrent();
	if (_selectionProjectionPending || !contentPairCurrent)
	{
		_selectionProjectionPending = true;
		_contentProjectionPending = true;
	}
}

void TabControl::ObserveSelectedContentProjection(TabItem* item)
{
	_selectedContentProjectionObservation.Disconnect();
	_observedSelectedContentItem = item;
	if (!item) return;
	const ControlWeakReference ownerLifetime(this);
	const ControlWeakReference itemLifetime(item);
	_selectedContentProjectionObservation =
		item->OnPropertyValueChanged.Subscribe(
			[ownerLifetime, itemLifetime](
				DependencyObject*,
				const DependencyPropertyChangedEventArgs& args)
			{
				if (args.Property != &ContentControl::ContentProperty()
					&& args.Property
						!= &ContentControl::ContentTemplateProperty())
					return;
				auto* owner = dynamic_cast<TabControl*>(
					ownerLifetime.Get());
				auto* selected = dynamic_cast<TabItem*>(
					itemLifetime.Get());
				if (!owner || !selected
					|| owner->_observedSelectedContentItem.Get() != selected
					|| owner->GetItem(owner->SelectedIndex) != selected)
					return;
				owner->_selectionProjectionPending = true;
				owner->_contentProjectionPending = true;
				owner->SynchronizeSelectionProjection();
				owner = dynamic_cast<TabControl*>(ownerLifetime.Get());
				if (!owner) return;
				owner->SynchronizeSelectedContentHost();
				owner = dynamic_cast<TabControl*>(ownerLifetime.Get());
				if (owner) owner->RequestLayout();
			});
}

bool TabControl::HasProjectedVisualContent(
	const TabItem* item) const noexcept
{
	return item
		&& _projectedVisualItem.Get() == item
		&& _projectedVisualContent.Get() != nullptr;
}

void TabControl::RestoreProjectedVisualContent()
{
	if (_restoringContentProjection)
	{
		_contentProjectionPending = true;
		return;
	}
	struct ProjectionRestoreScope final
	{
		ControlWeakReference OwnerLifetime;
		bool* Restoring = nullptr;
		bool* Synchronizing = nullptr;
		bool OwnsSynchronization = false;

		ProjectionRestoreScope(
			Control& owner, bool& restoring, bool& synchronizing)
			: OwnerLifetime(&owner), Restoring(&restoring),
			Synchronizing(&synchronizing),
			OwnsSynchronization(!synchronizing)
		{
			*Restoring = true;
			if (OwnsSynchronization) *Synchronizing = true;
		}
		~ProjectionRestoreScope()
		{
			auto* owner = dynamic_cast<TabControl*>(OwnerLifetime.Get());
			if (!owner) return;
			*Restoring = false;
			if (OwnsSynchronization) *Synchronizing = false;
			owner->CompleteDeferredTemplateAbort();
		}
	} restoreScope(
		*this, _restoringContentProjection, _synchronizingContentProjection);
	// The page moves from the central host back to its TabItem inside the same
	// Window; keep its presentation source (and layout caches) across the move.
	const ScopedPresentationWindowTransfer windowTransfer;
	const ControlWeakReference ownerLifetime(this);

	auto* host = dynamic_cast<ContentPresenter*>(
		_selectedContentHost.Get());
	auto* item = dynamic_cast<TabItem*>(_projectedVisualItem.Get());
	auto* visual = _projectedVisualContent.Get();
	const ControlWeakReference hostLifetime(host);
	const ControlWeakReference itemLifetime(item);
	const ControlWeakReference visualLifetime(visual);
	_projectedVisualItem.Reset();
	_projectedVisualContent.Reset();
	_contentProjectionPending = true;
	if (!host || !visual || visual->GetVisualParent() != host
		|| host->IndexOfVisualChild(visual) < 0) return;

	// A page parked outside the selected-content host is not part of the
	// presentation tree. Suppress it before detaching so native descendants
	// (WebView2, Media Foundation, and similar composition surfaces) can hide
	// while they still resolve the owning Window.
	std::exception_ptr notificationError;
	try
	{
		cui::framework::TemplateAccess::SetPresentationSuppressed(
			*visual, true);
	}
	catch (...)
	{
		notificationError = std::current_exception();
	}
	if (ownerLifetime.Get() != this)
	{
		if (notificationError)
			std::rethrow_exception(notificationError);
		return;
	}
	host = dynamic_cast<ContentPresenter*>(hostLifetime.Get());
	visual = visualLifetime.Get();
	if (!host || !visual || visual->GetVisualParent() != host
		|| host->IndexOfVisualChild(visual) < 0)
	{
		if (notificationError)
			std::rethrow_exception(notificationError);
		return;
	}

	bool ownershipCommit = false;
	std::exception_ptr detachError;
	auto owner = cui::framework::TreeAccess::DetachVisualChild(
		*host, visual, &ownershipCommit, &detachError);
	if (!notificationError) notificationError = detachError;
	if (!owner)
	{
		if (notificationError)
			std::rethrow_exception(notificationError);
		return;
	}

	item = dynamic_cast<TabItem*>(itemLifetime.Get());
	const bool itemCanReceive = item
		&& !item->ContentControl::GetVisualContent()
		&& item->GetContent().Empty()
		&& !item->GetContentTemplate();
	if (itemCanReceive)
	{
		try
		{
			if (!item->RestoreVisualContentFromProjection(owner) && owner
				&& !notificationError)
				notificationError = std::make_exception_ptr(
					std::logic_error(
						"TabItem rejected its restored visual content"));
		}
		catch (...)
		{
			if (!notificationError)
				notificationError = std::current_exception();
		}
	}
	else if (owner && owner->GetLogicalParent())
	{
		std::exception_ptr parentError;
		try
		{
			(void)cui::framework::TreeAccess::
				SetLogicalParentPreservingOwnership(
					owner, nullptr, &parentError);
		}
		catch (...)
		{
			parentError = std::current_exception();
		}
		if (!notificationError) notificationError = parentError;
	}
	if (notificationError)
		std::rethrow_exception(notificationError);
}

void TabControl::SynchronizeSelectedContentHost()
{
	if (_synchronizingSelectionProjection)
	{
		_contentProjectionPending = true;
		return;
	}
	if (_synchronizingContentProjection)
	{
		_contentProjectionPending = true;
		return;
	}
	_synchronizingContentProjection = true;
	// Page visuals move between their TabItem and PART_SelectedContentHost,
	// both inside this Window. Without a transfer scope every switch would walk
	// both complete pages twice (Window released, then re-acquired) and discard
	// every descendant's measure cache.
	const ScopedPresentationWindowTransfer windowTransfer;
	const ControlWeakReference ownerLifetime(this);
	try
	{
		for (size_t pass = 0; pass < 32; ++pass)
		{
			_contentProjectionPending = false;
			auto* part = FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_SelectedContentHost"));
			auto* host = dynamic_cast<ContentPresenter*>(part);
			if (part && !host)
				throw std::logic_error(
					"TabControl PART_SelectedContentHost must be a ContentPresenter");
			if (host && !host->ClipToBounds)
			{
				const ControlWeakReference hostLifetime(host);
				(void)host->TrySetCurrentPropertyValue(
					Control::ClipToBoundsProperty(), BindingValue(true));
				if (ownerLifetime.Get() != this) return;
				host = dynamic_cast<ContentPresenter*>(hostLifetime.Get());
				if (!host || FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"PART_SelectedContentHost")) != host)
				{
					_contentProjectionPending = true;
					continue;
				}
			}
			auto* selected = GetItem(SelectedIndex);
			auto* projectedHost = dynamic_cast<ContentPresenter*>(
				_selectedContentHost.Get());
			auto* projectedItem = dynamic_cast<TabItem*>(
				_projectedVisualItem.Get());
			auto* projectedVisual = _projectedVisualContent.Get();
			auto* localVisual = selected
				? selected->ContentControl::GetVisualContent() : nullptr;
			const bool projectionIsCurrent = host
				&& projectedHost == host
				&& projectedItem == selected
				&& projectedVisual
				&& projectedVisual->GetVisualParent() == host
				&& host->IndexOfVisualChild(projectedVisual) >= 0
				&& !localVisual;

			if (projectedVisual && !projectionIsCurrent)
			{
				RestoreProjectedVisualContent();
				if (ownerLifetime.Get() != this) return;
				// Suppression and detach publish synchronous callbacks. Resolve the
				// template part and selected item again before using either snapshot.
				continue;
			}
			_selectedContentHost = host;
			if (!host || !selected)
			{
				if (!_contentProjectionPending)
				{
					_synchronizingContentProjection = false;
					return;
				}
				continue;
			}

			if (projectionIsCurrent)
			{
				if (!_contentProjectionPending)
				{
					_synchronizingContentProjection = false;
					return;
				}
				continue;
			}

			localVisual = selected->ContentControl::GetVisualContent();
			if (localVisual)
			{
				// SelectedContent bindings must expose the data lane only. Clear
				// any stale bound value before the visual ownership transaction.
				if (!host->GetContent().Empty())
				{
					(void)host->TrySetCurrentPropertyValue(
						ContentPresenter::ContentProperty(), BindingValue{});
					if (ownerLifetime.Get() != this) return;
					_contentProjectionPending = true;
					continue;
				}
				if (host->GetContentTemplate())
				{
					(void)host->TrySetCurrentPropertyValue(
						ContentPresenter::ContentTemplateProperty(),
						BindingValue(ItemTemplateReference{}));
					if (ownerLifetime.Get() != this) return;
					_contentProjectionPending = true;
					continue;
				}
				if (auto* unexpected = host->GetVisualContent())
					throw std::logic_error(
						unexpected == projectedVisual
							? "TabControl selected visual projection is stale"
							: "TabControl selected content host already owns visual content");

				// Detach publishes VisualParentChanged synchronously. Freeze every
				// identity used by the transfer before that callback can replace the
				// selected page or destroy the current template host.
				const int selectedIndex = SelectedIndex;
				const ControlWeakReference hostLifetime(host);
				const ControlWeakReference selectedLifetime(selected);
				const ControlWeakReference visualLifetime(localVisual);
				auto owner = selected->DetachVisualContentForProjection();
				auto restoreDetachedOwner = [&]()
				{
					if (!owner) return true;
					auto* liveSelected = dynamic_cast<TabItem*>(
						selectedLifetime.Get());
					auto* liveVisual = visualLifetime.Get();
					if (!liveSelected || !liveVisual
						|| owner.get() != liveVisual
						|| liveSelected->ContentControl::GetVisualContent()
						|| !liveSelected->GetContent().Empty()
						|| liveSelected->GetContentTemplate())
						return false;
					if (liveSelected->RestoreVisualContentFromProjection(owner))
						return true;
					// A failed attach can still transfer ownership re-entrantly.
					return !owner;
				};
				auto releaseTransferredPresentation =
					[&](bool projectionOwnerAlive)
					{
						if (owner) return;
						auto* liveVisual = visualLifetime.Get();
						if (!liveVisual) return;
						auto* liveSelected = dynamic_cast<TabItem*>(
							selectedLifetime.Get());
						const bool sourceOwnsVisual = liveSelected
							&& liveSelected->ContentControl::GetVisualContent()
								== liveVisual;
						if (projectionOwnerAlive && sourceOwnsVisual) return;
						// Selection synchronization suppresses a page before central
						// projection. Once a callback transfers that page outside this
						// TabControl (or destroys the owner), no later pass can release
						// the stale gate on behalf of its new owner.
						cui::framework::TemplateAccess::
							SetPresentationSuppressed(*liveVisual, false);
					};
				if (ownerLifetime.Get() != this)
				{
					if (!restoreDetachedOwner())
						throw std::logic_error(
							"TabControl owner died before detached page visual could be restored");
					releaseTransferredPresentation(false);
					return;
				}
				host = dynamic_cast<ContentPresenter*>(hostLifetime.Get());
				selected = dynamic_cast<TabItem*>(selectedLifetime.Get());
				localVisual = visualLifetime.Get();
				const bool transferTargetIsCurrent = owner
					&& owner.get() == localVisual
					&& host && selected
					&& SelectedIndex == selectedIndex
					&& GetItem(selectedIndex) == selected
					&& FindDeclarativeTemplatePart(
						MakeTemplatePartToken(L"PART_SelectedContentHost")) == host
					&& !selected->ContentControl::GetVisualContent()
					&& host->GetContent().Empty()
					&& !host->GetContentTemplate()
					&& !host->GetVisualContent();
				if (!transferTargetIsCurrent)
				{
					_contentProjectionPending = true;
					// The callback invalidated the destination after ownership had
					// left the page. Park the still-owned visual back on its live
					// source before restarting convergence.
					if (!restoreDetachedOwner())
						throw std::logic_error(
							"TabControl detached page visual could not be restored");
					if (ownerLifetime.Get() != this)
					{
						releaseTransferredPresentation(false);
						return;
					}
					releaseTransferredPresentation(true);
					if (ownerLifetime.Get() != this) return;
					continue;
				}
				try
				{
					(void)cui::framework::TreeAccess::
						InsertOwnedVisualChildPreserving(
							*host, host->VisualChildCount(), owner, selected);
				}
				catch (...)
				{
					auto* live = visualLifetime.Get();
					auto* liveHost = dynamic_cast<ContentPresenter*>(
						hostLifetime.Get());
					auto* liveSelected = dynamic_cast<TabItem*>(
						selectedLifetime.Get());
					if (ownerLifetime.Get() != this) throw;
					if (live && liveHost
						&& live->GetVisualParent() == liveHost
						&& liveHost->IndexOfVisualChild(live) >= 0)
					{
						_selectedContentHost = liveHost;
						_projectedVisualItem = liveSelected;
						_projectedVisualContent = live;
					}
					else if (owner && liveSelected)
						(void)liveSelected->RestoreVisualContentFromProjection(
							owner);
					throw;
				}
				auto* live = visualLifetime.Get();
				host = dynamic_cast<ContentPresenter*>(hostLifetime.Get());
				selected = dynamic_cast<TabItem*>(selectedLifetime.Get());
				if (ownerLifetime.Get() != this) return;
				if (!host || !selected || !live
					|| live->GetVisualParent() != host
					|| live->GetLogicalParent() != selected
					|| host->IndexOfVisualChild(live) < 0)
					throw std::logic_error(
						"TabControl selected visual projection did not commit");
				_projectedVisualItem = selected;
				_projectedVisualContent = live;
				// The central host is the only presentation lane for page visuals.
				// Release the parking gate only after visual ownership commits.
				cui::framework::TemplateAccess::SetPresentationSuppressed(
					*live, false);
				if (ownerLifetime.Get() != this) return;
			}

			if (!_contentProjectionPending)
			{
				_synchronizingContentProjection = false;
				return;
			}
		}
		throw std::logic_error(
			"TabControl selected content projection did not converge");
	}
	catch (...)
	{
		if (ownerLifetime.Get() != this) throw;
		_synchronizingContentProjection = false;
		_contentProjectionPending = true;
		throw;
	}
}

void TabControl::SynchronizeSelectionProjection()
{
	if (_synchronizingSelectionProjection)
	{
		_selectionProjectionPending = true;
		_contentProjectionPending = true;
		return;
	}
	_synchronizingSelectionProjection = true;
	const ControlWeakReference ownerLifetime(this);
	try
	{
		for (size_t pass = 0; pass < 32; ++pass)
		{
			_selectionProjectionPending = false;
			const int selected = SelectedIndex;
			auto* selectedItem = GetItem(selected);
			BindingValue desiredContent;
			ItemTemplateReference desiredContentTemplate;
			DataTypeToken desiredContentType;
			const bool selectedHasVisual = selectedItem
				&& (HasProjectedVisualContent(selectedItem)
					|| selectedItem->ContentControl::GetVisualContent());
			if (selectedItem && !selectedHasVisual)
			{
				desiredContent = selectedItem->GetContent();
				desiredContentTemplate = selectedItem->GetContentTemplate()
					? selectedItem->GetContentTemplate() : _contentTemplate;
				desiredContentType = selectedItem->GetContentTypeToken();
			}
			const bool identityChanged =
				_selectedTabIdentity != selectedItem;
			const bool selectedContentChanged =
				!BindingItemValuesEqual(_selectedContent, desiredContent);
			const bool selectedTemplateChanged =
				_selectedContentTemplate != desiredContentTemplate;
			const bool selectedPairChanged =
				selectedContentChanged || selectedTemplateChanged;
			if (identityChanged || selectedPairChanged)
			{
				_contentProjectionPending = true;
				if (_projectedVisualContent.Get())
				{
					// Vacate the central host before publishing a data lane. A
					// ContentPresenter cannot accept data Content while it still owns
					// the previous page's visual child.
					RestoreProjectedVisualContent();
					if (ownerLifetime.Get() != this) return;
					_selectionProjectionPending = true;
					continue;
				}
			}

			// Content and ContentTemplate are independent TemplateBindings, but a
			// ContentPresenter validates and materializes them as one pair. Vacate
			// the old data lane before either selected-value DP is published, then
			// install the selected page's AOT DataType token while the host is empty.
			// Each operation can synchronously run user code, so every subsequent
			// access is made from weak and selection snapshots on a fresh pass.
			auto* part = FindDeclarativeTemplatePart(
				MakeTemplatePartToken(L"PART_SelectedContentHost"));
			auto* dataHost = dynamic_cast<ContentPresenter*>(part);
			if (part && !dataHost)
				throw std::logic_error(
					"TabControl PART_SelectedContentHost must be a ContentPresenter");
			const bool hostTypeChanged = dataHost
				&& dataHost->GetContentTypeToken() != desiredContentType;
			if (dataHost && (selectedPairChanged || hostTypeChanged))
			{
				const ControlWeakReference hostLifetime(dataHost);
				if (selectedContentChanged
					&& !dataHost->GetContent().Empty())
				{
					(void)dataHost->TrySetCurrentPropertyValue(
						ContentPresenter::ContentProperty(), BindingValue{});
					if (ownerLifetime.Get() != this) return;
					dataHost = dynamic_cast<ContentPresenter*>(
						hostLifetime.Get());
					if (!dataHost || FindDeclarativeTemplatePart(
							MakeTemplatePartToken(L"PART_SelectedContentHost"))
							!= dataHost
						|| SelectedIndex != selected
						|| GetItem(selected) != selectedItem)
					{
						_selectionProjectionPending = true;
						continue;
					}
					if (!dataHost->GetContent().Empty())
						throw std::logic_error(
							"TabControl selected data content did not vacate");
					_selectionProjectionPending = true;
					continue;
				}
				if (selectedTemplateChanged
					&& dataHost->GetContentTemplate())
				{
					(void)dataHost->TrySetCurrentPropertyValue(
						ContentPresenter::ContentTemplateProperty(),
						BindingValue(ItemTemplateReference{}));
					if (ownerLifetime.Get() != this) return;
					dataHost = dynamic_cast<ContentPresenter*>(
						hostLifetime.Get());
					if (!dataHost || FindDeclarativeTemplatePart(
							MakeTemplatePartToken(L"PART_SelectedContentHost"))
							!= dataHost
						|| SelectedIndex != selected
						|| GetItem(selected) != selectedItem)
					{
						_selectionProjectionPending = true;
						continue;
					}
					if (dataHost->GetContentTemplate())
						throw std::logic_error(
							"TabControl selected data template did not vacate");
					_selectionProjectionPending = true;
					continue;
				}
				if (dataHost->GetContentTypeToken() != desiredContentType)
				{
					dataHost->SetContentTypeToken(desiredContentType);
					if (ownerLifetime.Get() != this) return;
					dataHost = dynamic_cast<ContentPresenter*>(
						hostLifetime.Get());
					if (!dataHost || FindDeclarativeTemplatePart(
							MakeTemplatePartToken(L"PART_SelectedContentHost"))
							!= dataHost
						|| SelectedIndex != selected
						|| GetItem(selected) != selectedItem)
					{
						_selectionProjectionPending = true;
						continue;
					}
					if (dataHost->GetContentTypeToken()
						!= desiredContentType)
						throw std::logic_error(
							"TabControl selected content DataType did not commit");
					_selectionProjectionPending = true;
					continue;
				}
			}

			for (int index = 0;
				index < static_cast<int>(ItemCount()); ++index)
			{
				auto* page = GetItem(index);
				if (!page) continue;
				const ControlWeakReference pageLifetime(page);
				page->SetTabStripPlacementProjection(_tabStripPlacement);
				if (ownerLifetime.Get() != this) return;
				page = dynamic_cast<TabItem*>(pageLifetime.Get());
				if (!page || GetItem(index) != page
					|| SelectedIndex != selected
					|| GetItem(selected) != selectedItem
					|| _selectionProjectionPending)
				{
					_selectionProjectionPending = true;
					break;
				}
				page->SetCurrentIsSelected(index == selected);
				if (ownerLifetime.Get() != this) return;
				page = dynamic_cast<TabItem*>(pageLifetime.Get());
				if (!page || GetItem(index) != page
					|| SelectedIndex != selected
					|| GetItem(selected) != selectedItem
					|| _selectionProjectionPending)
				{
					_selectionProjectionPending = true;
					break;
				}
				if (auto* visual = page->GetVisualContent())
				{
					const bool isPresented = index == selected
						&& HasProjectedVisualContent(page)
						&& visual->GetVisualParent()
							== _selectedContentHost.Get();
					cui::framework::TemplateAccess::SetPresentationSuppressed(
						*visual, !isPresented);
					if (ownerLifetime.Get() != this) return;
				}
				if (SelectedIndex != selected
					|| GetItem(selected) != selectedItem
					|| _selectionProjectionPending)
				{
					_selectionProjectionPending = true;
					break;
				}
			}

			if (_selectionProjectionPending
				|| SelectedIndex != selected
				|| GetItem(selected) != selectedItem)
				continue;
			RefreshSelectedContentProjection();
			if (ownerLifetime.Get() != this) return;
			if (!_selectionProjectionPending
				&& SelectedIndex == selected
				&& GetItem(selected) == selectedItem
				&& !selectedHasVisual)
			{
				part = FindDeclarativeTemplatePart(
					MakeTemplatePartToken(L"PART_SelectedContentHost"));
				dataHost = dynamic_cast<ContentPresenter*>(part);
				if (part && !dataHost)
					throw std::logic_error(
						"TabControl PART_SelectedContentHost must be a ContentPresenter");
				if (dataHost
					&& dataHost->GetContentTemplate()
						!= desiredContentTemplate)
				{
					const ControlWeakReference hostLifetime(dataHost);
					(void)dataHost->TrySetCurrentPropertyValue(
						ContentPresenter::ContentTemplateProperty(),
						BindingValue(desiredContentTemplate));
					if (ownerLifetime.Get() != this) return;
					dataHost = dynamic_cast<ContentPresenter*>(
						hostLifetime.Get());
					if (!dataHost || FindDeclarativeTemplatePart(
							MakeTemplatePartToken(L"PART_SelectedContentHost"))
							!= dataHost
						|| SelectedIndex != selected
						|| GetItem(selected) != selectedItem)
					{
						_selectionProjectionPending = true;
						continue;
					}
					if (dataHost->GetContentTemplate()
						!= desiredContentTemplate)
						throw std::logic_error(
							"TabControl selected data template did not commit");
					_selectionProjectionPending = true;
					continue;
				}
				if (dataHost && !BindingItemValuesEqual(
					dataHost->GetContent(), desiredContent))
				{
					const ControlWeakReference hostLifetime(dataHost);
					(void)dataHost->TrySetCurrentPropertyValue(
						ContentPresenter::ContentProperty(), desiredContent);
					if (ownerLifetime.Get() != this) return;
					dataHost = dynamic_cast<ContentPresenter*>(
						hostLifetime.Get());
					if (!dataHost || FindDeclarativeTemplatePart(
							MakeTemplatePartToken(L"PART_SelectedContentHost"))
							!= dataHost
						|| SelectedIndex != selected
						|| GetItem(selected) != selectedItem)
					{
						_selectionProjectionPending = true;
						continue;
					}
					if (!BindingItemValuesEqual(
						dataHost->GetContent(), desiredContent))
						throw std::logic_error(
							"TabControl selected data content did not commit");
					_selectionProjectionPending = true;
					continue;
				}
			}
			if (!_selectionProjectionPending
				&& SelectedIndex == selected
				&& GetItem(selected) == selectedItem)
			{
				const bool reconnectObservation =
					_observedSelectedContentItem.Get() != selectedItem
					|| !_selectedContentProjectionObservation.Connected();
				_selectedTabIdentity = selectedItem;
				if (reconnectObservation)
					ObserveSelectedContentProjection(selectedItem);
				_synchronizingSelectionProjection = false;
				return;
			}
		}
		throw std::logic_error(
			"TabControl selection projection did not converge");
	}
	catch (...)
	{
		if (ownerLifetime.Get() != this) throw;
		_synchronizingSelectionProjection = false;
		_selectionProjectionPending = true;
		_contentProjectionPending = true;
		throw;
	}
}

void TabControl::OnSelectedIndexChanged(int, int)
{
	SynchronizeSelectionProjection();
	SynchronizeSelectedContentHost();
	RequestLayout();
	InvalidateVisual();
}

void TabControl::PrepareItemMutation()
{
	_selectedContentProjectionObservation.Disconnect();
	_observedSelectedContentItem.Reset();
	RestoreProjectedVisualContent();
	_selectedTabIdentity = GetItem(SelectedIndex);
}

void TabControl::ReconcileItemsAfterMutation(
	TabItem* previouslySelectedItem)
{
	int explicitlySelected = -1;
	for (int index = 0; index < static_cast<int>(ItemCount()); ++index)
	{
		auto* item = GetItem(index);
		if (item && item != previouslySelectedItem && item->IsSelected)
			explicitlySelected = index;
	}
	Selector::OnAuthoredItemsChanged();
	if (ItemCount() == 0 && previouslySelectedItem)
	{
		// Selector keeps a non-negative index for an initially empty authored
		// collection so XAML may declare SelectedIndex before its Items. Once an
		// established selected TabItem is actually removed, however, the empty
		// collection is a committed selection change and cannot retain that
		// future-index sentinel.
		SetCurrentSelectedIndex(-1);
	}
	else if (explicitlySelected >= 0)
		SetCurrentSelectedIndex(explicitlySelected);
	else if (ItemCount() > 0 && SelectedIndex < 0)
		SetCurrentSelectedIndex(0);
	SynchronizeSelectionProjection();
	SynchronizeSelectedContentHost();
	RequestLayout();
	InvalidateVisual();
}

TabItem* TabControl::AddItem(std::unique_ptr<TabItem> page)
{
	return InsertItem(static_cast<int>(ItemCount()), std::move(page));
}

TabItem* TabControl::InsertItem(
	int index, std::unique_ptr<TabItem> page)
{
	if (!page) throw std::invalid_argument("cannot add a null TabItem");
	if (index < 0 || index > static_cast<int>(ItemCount()))
		throw std::out_of_range("TabItem index is out of range");
	return static_cast<TabItem*>(InsertItemControl(
		static_cast<size_t>(index), std::move(page)));
}

TabItem* TabControl::GetItem(int index) const noexcept
{
	return index < 0 ? nullptr : static_cast<TabItem*>(
		GetGeneratedItem(static_cast<size_t>(index)));
}

int TabControl::IndexOfItem(const TabItem* page) const noexcept
{
	if (!page) return -1;
	for (size_t index = 0; index < ItemCount(); ++index)
		if (GetGeneratedItem(index) == page) return static_cast<int>(index);
	return -1;
}

std::unique_ptr<Control> TabControl::DetachItemControlAt(size_t index)
{
	if (index >= ItemCount()) return {};
	const ControlWeakReference ownerLifetime(this);
	auto* originalTarget = dynamic_cast<TabItem*>(
		GetGeneratedItem(index));
	const ControlWeakReference targetLifetime(originalTarget);
	const ControlWeakReference originalContentLifetime(
		originalTarget ? originalTarget->GetVisualContent() : nullptr);

	auto releaseTransferredPresentation = [&]
	{
		auto* liveOwner = dynamic_cast<TabControl*>(ownerLifetime.Get());
		auto* target = dynamic_cast<TabItem*>(targetLifetime.Get());
		if (liveOwner && target && liveOwner->IndexOfItem(target) >= 0)
			return;
		auto releaseContent = [](Control* content)
		{
			if (!content) return;
			try
			{
				cui::framework::TemplateAccess::SetPresentationSuppressed(
					*content, false);
			}
			catch (...)
			{
				// Detach/transfer has committed. Visibility notification errors
				// cannot revoke the ownership outcome.
			}
		};
		auto* originalContent = originalContentLifetime.Get();
		releaseContent(originalContent);
		target = dynamic_cast<TabItem*>(targetLifetime.Get());
		if (target)
		{
			auto* currentContent = target->GetVisualContent();
			if (currentContent != originalContent)
				releaseContent(currentContent);
		}
		target = dynamic_cast<TabItem*>(targetLifetime.Get());
		auto* newOwner = target
			? dynamic_cast<TabControl*>(target->GetLogicalParent()) : nullptr;
		if (newOwner && newOwner != liveOwner)
		{
			const ControlWeakReference newOwnerLifetime(newOwner);
			try
			{
				newOwner->_contentProjectionPending = true;
				newOwner->SynchronizeSelectionProjection();
				if (auto* liveOwner = dynamic_cast<TabControl*>(
					newOwnerLifetime.Get()))
					liveOwner->RequestLayout();
			}
			catch (...) {}
		}
	};
	try
	{
		PrepareItemMutation();
	}
	catch (...)
	{
		releaseTransferredPresentation();
		throw;
	}

	auto* liveOwner = dynamic_cast<TabControl*>(ownerLifetime.Get());
	auto* target = dynamic_cast<TabItem*>(targetLifetime.Get());
	const int currentIndex = liveOwner
		? liveOwner->IndexOfItem(target) : -1;
	if (!liveOwner || !target || currentIndex < 0)
	{
		releaseTransferredPresentation();
		return {};
	}
	std::unique_ptr<Control> result;
	try
	{
		result = liveOwner->ItemsControl::DetachItemControlAt(
			static_cast<size_t>(currentIndex));
	}
	catch (...)
	{
		releaseTransferredPresentation();
		throw;
	}
	releaseTransferredPresentation();
	return result;
}

std::unique_ptr<TabItem> TabControl::DetachItemAt(int index)
{
	if (index < 0 || index >= static_cast<int>(ItemCount())) return {};
	auto detached = DetachItemControlAt(static_cast<size_t>(index));
	auto result = std::unique_ptr<TabItem>(
		static_cast<TabItem*>(detached.release()));
	return result;
}

std::unique_ptr<TabItem> TabControl::DetachItem(TabItem* page)
{
	return DetachItemAt(IndexOfItem(page));
}

bool TabControl::RemoveItemAt(int index)
{
	return DetachItemAt(index) != nullptr;
}

bool TabControl::RemoveItem(TabItem* page)
{
	return RemoveItemAt(IndexOfItem(page));
}

bool TabControl::MoveItem(int oldIndex, int newIndex)
{
	if (oldIndex < 0 || newIndex < 0
		|| oldIndex >= static_cast<int>(ItemCount())
		|| newIndex >= static_cast<int>(ItemCount())) return false;
	if (oldIndex == newIndex) return true;
	const ControlWeakReference movingLifetime(GetItem(oldIndex));
	const ControlWeakReference destinationLifetime(GetItem(newIndex));
	PrepareItemMutation();
	auto* moving = dynamic_cast<TabItem*>(movingLifetime.Get());
	auto* destination = dynamic_cast<TabItem*>(destinationLifetime.Get());
	const int currentOldIndex = IndexOfItem(moving);
	const int currentNewIndex = IndexOfItem(destination);
	if (!moving || !destination
		|| currentOldIndex < 0 || currentNewIndex < 0) return false;
	return MoveItemControl(
		static_cast<size_t>(currentOldIndex),
		static_cast<size_t>(currentNewIndex));
}

void TabControl::ClearItems()
{
	PrepareItemMutation();
	if (GetItemsSource()) SetItemsSource({});
	else ClearItemControls();
	// SelectedIndex may be authored before Items are populated, but clearing an
	// established collection is a committed selection change, not a pending
	// future index.
	SetCurrentSelectedIndex(-1);
}

bool TabControl::SelectItem(int index)
{
	return index >= 0 && index < static_cast<int>(ItemCount())
		&& (SelectedIndex == index || SelectIndex(index));
}

BindingValue TabControl::GetSelectedItem() const
{
	if (GetItemsSource()) return Selector::GetSelectedItem();
	auto* item = GetItem(SelectedIndex);
	return item ? BindingValue(item) : BindingValue{};
}

void TabControl::SetSelectedItem(const BindingValue& value)
{
	if (GetItemsSource())
	{
		Selector::SetSelectedItem(value);
		return;
	}
	if (value.Empty())
	{
		SetCurrentSelectedIndex(-1);
		return;
	}
	TabItem* item = nullptr;
	SetCurrentSelectedIndex(value.TryGet(item) ? IndexOfItem(item) : -1);
}

BindingValue TabControl::GetSelectedValue() const
{
	if (GetItemsSource()) return Selector::GetSelectedValue();
	auto* item = GetItem(SelectedIndex);
	if (!item) return {};
	if (!HasSelectedValuePath()) return BindingValue(item);
	BindingValue result;
	return TryReadSelectedValue(*item, result)
		? result : BindingValue{};
}

void TabControl::SetSelectedValue(const BindingValue& value)
{
	if (GetItemsSource())
	{
		Selector::SetSelectedValue(value);
		return;
	}
	if (value.Empty())
	{
		SetCurrentSelectedIndex(-1);
		return;
	}
	if (!HasSelectedValuePath())
	{
		TabItem* item = nullptr;
		SetCurrentSelectedIndex(value.TryGet(item) ? IndexOfItem(item) : -1);
		return;
	}
	for (int index = 0; index < static_cast<int>(ItemCount()); ++index)
	{
		BindingValue candidate;
		auto* item = GetItem(index);
		if (item && TryReadSelectedValue(*item, candidate)
			&& BindingItemValuesEqual(candidate, value))
		{
			SetCurrentSelectedIndex(index);
			return;
		}
	}
	SetCurrentSelectedIndex(-1);
}

std::unique_ptr<Panel> TabControl::CreateItemsHost() const
{
	return std::make_unique<TabItemsHost>(
		*const_cast<TabControl*>(this));
}

std::unique_ptr<Control> TabControl::BuildGeneratedItem(
	const BindingSourceReference& item,
	size_t,
	BindingPathObservation& observation)
{
	auto page = std::make_unique<TabItem>();
	cui::framework::StyleAccess::SetResourceKey(
		*page, GetItemContainerStyle());
	page->SetDataContext(item);
	page->SetHeader(BindingValue(GetDisplayMemberText(item)));
	page->SetContent(BindingValue(item));
	page->SetContentTemplate(GetItemTemplate());
	page->SetCompiledDisplayMemberPath(GetCompiledDisplayMemberPath());
#if CUI_ENABLE_DYNAMIC_XAML
	if (GetCompiledDisplayMemberPath().Empty())
		page->SetDisplayMemberPath(GetDisplayMemberPath());
#endif
	auto* pagePointer = page.get();
	std::weak_ptr<IBindingSource> itemIdentity = item.Shared();
	const auto compiledDisplayPath = GetCompiledDisplayMemberPath();
#if CUI_ENABLE_DYNAMIC_XAML
	const auto dynamicDisplayPath = GetDisplayMemberPath();
#endif
	observation = ObserveDisplayMemberPath(
		item,
		[pagePointer, itemIdentity, compiledDisplayPath
#if CUI_ENABLE_DYNAMIC_XAML
			, dynamicDisplayPath
#endif
		]
		{
			const auto source = itemIdentity.lock();
			if (!source) return;
			const BindingSourceReference item(source);
			if (!compiledDisplayPath.Empty())
				pagePointer->SetHeader(BindingValue(
					GetBindingRecordText(item, compiledDisplayPath)));
#if CUI_ENABLE_DYNAMIC_XAML
			else pagePointer->SetHeader(BindingValue(
				GetBindingRecordText(item, dynamicDisplayPath)));
#endif
		});
	return page;
}

void TabControl::OnBeforeGeneratedItemsRebuilt()
{
	_selectedContentProjectionObservation.Disconnect();
	_observedSelectedContentItem.Reset();
	RestoreProjectedVisualContent();
}

void TabControl::OnGeneratedItemsRebuilt()
{
	Selector::OnGeneratedItemsRebuilt();
	if (ItemCount() > 0 && SelectedIndex < 0)
		SetCurrentSelectedIndex(0);
	for (int index = 0; index < static_cast<int>(ItemCount()); ++index)
		if (auto* item = GetItem(index))
			cui::framework::TreeAccess::SetLogicalParent(*item, this);
	SynchronizeSelectionProjection();
	SynchronizeSelectedContentHost();
	RequestLayout();
}

bool TabControl::ValidateAuthoredItemControl(
	const Control& item, std::string& error) const
{
	if (dynamic_cast<const TabItem*>(&item)) return true;
	error = "TabControl Items can contain TabItem controls only";
	return false;
}

void TabControl::OnAuthoredItemsChanged() noexcept
{
	try
	{
		RestoreProjectedVisualContent();
		auto* identity = _selectedTabIdentity;
		PrepareItemMutation();
		ReconcileItemsAfterMutation(identity);
	}
	catch (...)
	{
		_selectedTabIdentity = nullptr;
	}
}

void TabControl::CompleteDeferredTemplateAbort() noexcept
{
	if (!_templateAbortDeferred || _restoringContentProjection) return;
	_templateAbortDeferred = false;
	try
	{
		// AbortControlTemplateApplication could not detach while the selected
		// page ownership transaction was publishing visibility callbacks. The
		// page is parked now, so discard the obsolete template root and leave
		// ApplyTemplate to materialize the latest effective Template value.
		auto obsoleteRoot = DetachVisualChildTemplateRoot();
		if (GetControlTemplateRoot())
			_templateAbortDeferred = true;
		else
			MarkControlTemplateRootDetached();
	}
	catch (...)
	{
		if (GetControlTemplateRoot())
			_templateAbortDeferred = true;
		else
			MarkControlTemplateRootDetached();
	}
	if (_templateAbortDeferred) RequestLayout();
}

std::unique_ptr<Control> TabControl::DetachVisualChildTemplateRoot()
{
	if (_restoringContentProjection)
	{
		// RestoreProjectedVisualContent owns the selected page transfer while
		// suppression notifications are being published. A nested template
		// detach cannot issue the same root ownership token: if its return value
		// is discarded, the root would destroy the still-projected page before
		// the outer transfer can park it back on its TabItem. Report no detach;
		// an enclosing template detach will continue after restoration, while an
		// independent callback can retry once the notification transaction ends.
		_contentProjectionPending = true;
		return {};
	}
	const ControlWeakReference ownerLifetime(this);
	RestoreProjectedVisualContent();
	if (ownerLifetime.Get() != this) return {};
	_selectedContentHost.Reset();
	_contentProjectionPending = true;
	return ItemsControl::DetachVisualChildTemplateRoot();
}

void TabControl::OnControlTemplatePresentationChanged()
{
	_selectedContentHost.Reset();
	_contentProjectionPending = true;
	RequestLayout();
}

void TabControl::OnTemplateChanged(
	const ControlTemplateReference& oldTemplate,
	const ControlTemplateReference& newTemplate)
{
	if (_restoringContentProjection && GetControlTemplateRoot())
	{
		_templateAbortDeferred = true;
		_contentProjectionPending = true;
	}
	Control::OnTemplateChanged(oldTemplate, newTemplate);
}

int TabControl::FindNextEligibleTab(
	int startIndex,
	int direction,
	bool wrap) const noexcept
{
	const int count = static_cast<int>(ItemCount());
	if (count <= 0 || direction == 0) return -1;
	int index = startIndex;
	for (int visited = 0; visited < count; ++visited)
	{
		index += direction;
		if (index < 0 || index >= count)
		{
			if (!wrap) return -1;
			index = index < 0 ? count - 1 : 0;
		}
		auto* item = GetItem(index);
		if (item && item->IsVisible
			&& item->IsEffectivelyEnabled()
			&& item->CanReceiveKeyboardFocus())
			return index;
	}
	return -1;
}

bool TabControl::FocusAndSelectItem(int index)
{
	auto* item = GetItem(index);
	if (!item || !item->IsVisible || !item->IsEffectivelyEnabled())
		return false;
	const ControlWeakReference itemLifetime(item);
	if (!SelectItem(index)) return false;
	item = dynamic_cast<TabItem*>(itemLifetime.Get());
	return item && item->Focus();
}

bool TabControl::ProcessTabNavigationKey(const InputReport& input)
{
	if (input.Kind != InputReportKind::KeyDown || ItemCount() == 0)
		return false;
	int start = SelectedIndex;
	int direction = 0;
	bool wrap = true;
	if (input.Key == Key::Tab
		&& input.HasModifier(ModifierKeys::Control))
		direction = input.HasModifier(ModifierKeys::Shift) ? -1 : 1;
	else if (input.Key == Key::Home)
	{
		start = -1;
		direction = 1;
		wrap = false;
	}
	else if (input.Key == Key::End)
	{
		start = static_cast<int>(ItemCount());
		direction = -1;
		wrap = false;
	}
	else
	{
		const bool vertical = IsVerticalStrip(_tabStripPlacement);
		if ((!vertical && input.Key == Key::Left)
			|| (vertical && input.Key == Key::Up))
			direction = -1;
		else if ((!vertical && input.Key == Key::Right)
			|| (vertical && input.Key == Key::Down))
			direction = 1;
	}
	if (direction == 0) return false;
	const int next = FindNextEligibleTab(start, direction, wrap);
	return next >= 0 && next != SelectedIndex
		&& FocusAndSelectItem(next);
}

bool TabControl::ProcessInput(const InputReport& input)
{
	if (ProcessTabNavigationKey(input))
	{
		auto args = input.CreateKeyEventArgs();
		OnKeyDown(this, args);
		return true;
	}
	return Selector::ProcessInput(input);
}
