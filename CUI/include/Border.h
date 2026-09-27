#pragma once

#include "Decorator.h"

#include <array>
#include <wrl/client.h>

/**
 * WPF-style Border decorator.
 *
 * Border is the only structural primitive that owns Padding and border chrome.
 * Panels remain layout-only elements and therefore cannot acquire these
 * members merely because the native implementation shares a Control host.
 */
class Border final : public Decorator
{
public:
	Border() { RegisterDependencyProperties(); }
	UIClass Type() override { return UIClass::UI_Border; }
	/** WPF identities: Border owns its brush and thickness chrome. */
	static const DependencyProperty& BorderBrushProperty();
	static const DependencyProperty& BackgroundProperty();
	static const DependencyProperty& BorderThicknessProperty();
	static const DependencyProperty& CornerRadiusProperty();
	static const DependencyProperty& PaddingProperty();
	PROPERTY(Thickness, Padding);
	GET(Thickness, Padding);
	SET(Thickness, Padding);
	PROPERTY(::CornerRadius, CornerRadius);
	GET(::CornerRadius, CornerRadius);
	SET(::CornerRadius, CornerRadius);
	static void RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	void EnsureBindingPropertiesRegistered() override
	{
		RegisterDependencyProperties();
	}
#endif

protected:
	const DependencyPropertyMetadata* ResolveExactDependencyPropertyMetadata(
		const DependencyProperty& property) const override;
	cui::core::Insets GetDecoratorInsets() const noexcept override;
	void OnRender() override;

private:
	static const DependencyPropertyMetadataRegistration&
		BackgroundPropertyMetadataRelation();

public:
	/**
	 * One retained device-independent fill geometry. Recycled virtualized rows
	 * render identically sized chrome every time they re-record; rebuilding the
	 * same ID2D1PathGeometry through the factory for every cell dominated the
	 * cost of re-recording a realized DataGrid page.
	 */
	struct GeometryCacheSlot final
	{
		std::array<float, 25> Key{};
		bool Valid = false;
		Microsoft::WRL::ComPtr<ID2D1PathGeometry> Geometry;
	};

private:
	GeometryCacheSlot _backgroundGeometryCache;
	GeometryCacheSlot _borderGeometryCache;
};
