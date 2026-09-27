#include "Border.h"
#include "Panel.h"

#include <Factory.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <wrl/client.h>

namespace
{
	using Microsoft::WRL::ComPtr;

	std::optional<cui::drawing::Brush> ConvertBorderBrush(
		const BindingValue& value)
	{
		cui::drawing::Brush brush;
		if (value.TryGet(brush)) return brush;
		D2D1_COLOR_F color{};
		if (value.TryGet(color))
			return cui::drawing::MakeSolidColorBrush(color);
		return std::nullopt;
	}

#if CUI_ENABLE_DESIGN_METADATA
	DependencyPropertyDesignMetadata BorderBrushDesign(
		int order,
		const wchar_t* displayName)
	{
		DependencyPropertyDesignMetadata design;
		design.Browsable = false;
		design.DisplayName = displayName;
		design.Category = L"Appearance";
		design.CategoryOrder = 200;
		design.Order = order;
		design.Editor = DependencyPropertyEditorKind::Text;
		design.Persistence = DependencyPropertyPersistence::Metadata;
		return design;
	}
#endif

	bool IsFiniteNonNegativeThickness(const Thickness& value) noexcept
	{
		return std::isfinite(value.Left) && value.Left >= 0.0f
			&& std::isfinite(value.Top) && value.Top >= 0.0f
			&& std::isfinite(value.Right) && value.Right >= 0.0f
			&& std::isfinite(value.Bottom) && value.Bottom >= 0.0f;
	}

	bool IsFiniteNonNegativeCornerRadius(
		const CornerRadius& value) noexcept
	{
		return std::isfinite(value.TopLeft) && value.TopLeft >= 0.0f
			&& std::isfinite(value.TopRight) && value.TopRight >= 0.0f
			&& std::isfinite(value.BottomRight) && value.BottomRight >= 0.0f
			&& std::isfinite(value.BottomLeft) && value.BottomLeft >= 0.0f;
	}

	struct BorderRadii final
	{
		float LeftTop = 0.0f;
		float TopLeft = 0.0f;
		float TopRight = 0.0f;
		float RightTop = 0.0f;
		float RightBottom = 0.0f;
		float BottomRight = 0.0f;
		float BottomLeft = 0.0f;
		float LeftBottom = 0.0f;
	};

	BorderRadii ResolveBorderRadii(
		const CornerRadius& radius,
		const Thickness& border,
		bool outer) noexcept
	{
		const float left = border.Left * 0.5f;
		const float top = border.Top * 0.5f;
		const float right = border.Right * 0.5f;
		const float bottom = border.Bottom * 0.5f;
		BorderRadii result;
		if (outer)
		{
			if (radius.TopLeft > 0.0f)
			{
				result.LeftTop = radius.TopLeft + left;
				result.TopLeft = radius.TopLeft + top;
			}
			if (radius.TopRight > 0.0f)
			{
				result.TopRight = radius.TopRight + top;
				result.RightTop = radius.TopRight + right;
			}
			if (radius.BottomRight > 0.0f)
			{
				result.RightBottom = radius.BottomRight + right;
				result.BottomRight = radius.BottomRight + bottom;
			}
			if (radius.BottomLeft > 0.0f)
			{
				result.BottomLeft = radius.BottomLeft + bottom;
				result.LeftBottom = radius.BottomLeft + left;
			}
			return result;
		}

		result.LeftTop = (std::max)(0.0f, radius.TopLeft - left);
		result.TopLeft = (std::max)(0.0f, radius.TopLeft - top);
		result.TopRight = (std::max)(0.0f, radius.TopRight - top);
		result.RightTop = (std::max)(0.0f, radius.TopRight - right);
		result.RightBottom =
			(std::max)(0.0f, radius.BottomRight - right);
		result.BottomRight =
			(std::max)(0.0f, radius.BottomRight - bottom);
		result.BottomLeft =
			(std::max)(0.0f, radius.BottomLeft - bottom);
		result.LeftBottom =
			(std::max)(0.0f, radius.BottomLeft - left);
		return result;
	}

	void AppendRoundedRectangle(
		ID2D1GeometrySink& sink,
		D2D1_RECT_F rect,
		const BorderRadii& radii)
	{
		const float width = (std::max)(0.0f, rect.right - rect.left);
		const float height = (std::max)(0.0f, rect.bottom - rect.top);

		D2D1_POINT_2F topLeft{
			rect.left + radii.LeftTop, rect.top };
		D2D1_POINT_2F topRight{
			rect.right - radii.RightTop, rect.top };
		D2D1_POINT_2F rightTop{
			rect.right, rect.top + radii.TopRight };
		D2D1_POINT_2F rightBottom{
			rect.right, rect.bottom - radii.BottomRight };
		D2D1_POINT_2F bottomRight{
			rect.right - radii.RightBottom, rect.bottom };
		D2D1_POINT_2F bottomLeft{
			rect.left + radii.LeftBottom, rect.bottom };
		D2D1_POINT_2F leftBottom{
			rect.left, rect.bottom - radii.BottomLeft };
		D2D1_POINT_2F leftTop{
			rect.left, rect.top + radii.TopLeft };

		if (topLeft.x > topRight.x)
		{
			const float sum = radii.LeftTop + radii.RightTop;
			const float partition = sum > 0.0f
				? radii.LeftTop / sum * width : 0.0f;
			topLeft.x = topRight.x = rect.left + partition;
		}
		if (rightTop.y > rightBottom.y)
		{
			const float sum = radii.TopRight + radii.BottomRight;
			const float partition = sum > 0.0f
				? radii.TopRight / sum * height : 0.0f;
			rightTop.y = rightBottom.y = rect.top + partition;
		}
		if (bottomRight.x < bottomLeft.x)
		{
			const float sum = radii.LeftBottom + radii.RightBottom;
			const float partition = sum > 0.0f
				? radii.LeftBottom / sum * width : 0.0f;
			bottomLeft.x = bottomRight.x = rect.left + partition;
		}
		if (leftBottom.y < leftTop.y)
		{
			const float sum = radii.TopLeft + radii.BottomLeft;
			const float partition = sum > 0.0f
				? radii.TopLeft / sum * height : 0.0f;
			leftTop.y = leftBottom.y = rect.top + partition;
		}

		const auto appendArc = [&sink](
			D2D1_POINT_2F target, float radiusX, float radiusY)
		{
			if (radiusX <= 0.0f && radiusY <= 0.0f)
			{
				sink.AddLine(target);
				return;
			}
			sink.AddArc(D2D1::ArcSegment(
				target,
				D2D1::SizeF(
					(std::max)(0.0f, radiusX),
					(std::max)(0.0f, radiusY)),
				0.0f,
				D2D1_SWEEP_DIRECTION_CLOCKWISE,
				D2D1_ARC_SIZE_SMALL));
		};

		sink.BeginFigure(topLeft, D2D1_FIGURE_BEGIN_FILLED);
		sink.AddLine(topRight);
		appendArc(
			rightTop, rect.right - topRight.x, rightTop.y - rect.top);
		sink.AddLine(rightBottom);
		appendArc(
			bottomRight, rect.right - bottomRight.x,
			rect.bottom - rightBottom.y);
		sink.AddLine(bottomLeft);
		appendArc(
			leftBottom, bottomLeft.x - rect.left,
			rect.bottom - leftBottom.y);
		sink.AddLine(leftTop);
		appendArc(
			topLeft, topLeft.x - rect.left, leftTop.y - rect.top);
		sink.EndFigure(D2D1_FIGURE_END_CLOSED);
	}

	ComPtr<ID2D1PathGeometry> CreateBorderGeometry(
		D2D1_RECT_F outerRect,
		const BorderRadii& outerRadii,
		const D2D1_RECT_F* innerRect,
		const BorderRadii* innerRadii)
	{
		ComPtr<ID2D1PathGeometry> geometry;
		auto* factory = Factory::D2DFactory();
		if (!factory
			|| FAILED(factory->CreatePathGeometry(&geometry))) return {};
		ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(geometry->Open(&sink))) return {};
		sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
		AppendRoundedRectangle(*sink.Get(), outerRect, outerRadii);
		if (innerRect && innerRadii
			&& innerRect->right > innerRect->left
			&& innerRect->bottom > innerRect->top)
			AppendRoundedRectangle(*sink.Get(), *innerRect, *innerRadii);
		if (FAILED(sink->Close())) return {};
		return geometry;
	}

	ComPtr<ID2D1PathGeometry> CachedBorderGeometry(
		Border::GeometryCacheSlot& cache,
		D2D1_RECT_F outerRect,
		const BorderRadii& outerRadii,
		const D2D1_RECT_F* innerRect,
		const BorderRadii* innerRadii)
	{
		const bool hasInner = innerRect && innerRadii;
		const D2D1_RECT_F inner = hasInner ? *innerRect : D2D1_RECT_F{};
		const BorderRadii innerR = hasInner ? *innerRadii : BorderRadii{};
		const std::array<float, 25> key{
			outerRect.left, outerRect.top, outerRect.right, outerRect.bottom,
			outerRadii.LeftTop, outerRadii.TopLeft, outerRadii.TopRight,
			outerRadii.RightTop, outerRadii.RightBottom, outerRadii.BottomRight,
			outerRadii.BottomLeft, outerRadii.LeftBottom,
			hasInner ? 1.0f : 0.0f,
			inner.left, inner.top, inner.right, inner.bottom,
			innerR.LeftTop, innerR.TopLeft, innerR.TopRight, innerR.RightTop,
			innerR.RightBottom, innerR.BottomRight, innerR.BottomLeft,
			innerR.LeftBottom };
		if (cache.Valid && cache.Geometry && cache.Key == key)
			return cache.Geometry;
		auto geometry = CreateBorderGeometry(
			outerRect, outerRadii, innerRect, innerRadii);
		cache.Key = key;
		cache.Geometry = geometry;
		cache.Valid = geometry != nullptr;
		return geometry;
	}
}

const DependencyProperty& Border::BorderBrushProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Border, cui::drawing::Brush> options;
		options.DefaultValue = cui::drawing::NoBrush();
		options.Flags = DependencyPropertyFlags::AffectsRender;
		options.Convert = ConvertBorderBrush;
		CUI_DESIGN_METADATA_ONLY(
		options.Design = BorderBrushDesign(30, L"BorderBrush");
		)
		return DependencyPropertyRegistry::RegisterStatic<
			Border, cui::drawing::Brush>(
				DependencyPropertyRegistrationLiteral(L"BorderBrush"),
				std::move(options));
	}();
	return *registration;
}

const DependencyPropertyMetadataRegistration&
Border::BackgroundPropertyMetadataRelation()
{
	static const DependencyPropertyMetadataRegistration relation = []
	{
		DependencyPropertyOptions<Border, cui::drawing::Brush> options;
		options.DefaultValue = cui::drawing::NoBrush();
		options.Flags = DependencyPropertyFlags::AffectsRender;
		options.Convert = ConvertBorderBrush;
		CUI_DESIGN_METADATA_ONLY(
		options.Design = BorderBrushDesign(10, L"Background");
		)
		return DependencyPropertyRegistry::AddOwnerStatic<
			Border, cui::drawing::Brush>(
				Panel::BackgroundProperty(), std::move(options));
	}();
	return relation;
}

const DependencyProperty& Border::BackgroundProperty()
{
	return BackgroundPropertyMetadataRelation().Property();
}

const DependencyProperty& Border::BorderThicknessProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Border, Thickness> options;
		options.DefaultValue = Thickness{};
		options.Flags = DependencyPropertyFlags::AffectsMeasure
			| DependencyPropertyFlags::AffectsRender;
		options.Validate = IsFiniteNonNegativeThickness;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 200;
		options.Design.Order = 40;
		options.Design.Persistence =
			DependencyPropertyPersistence::Metadata;
		options.Design.Editor =
			DependencyPropertyEditorKind::Thickness;
		)
		return DependencyPropertyRegistry::RegisterStatic<Border, Thickness>(
			DependencyPropertyRegistrationLiteral(L"BorderThickness"),
			std::move(options));
	}();
	return *registration;
}

const DependencyProperty& Border::CornerRadiusProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Border, ::CornerRadius> options;
		options.DefaultValue = ::CornerRadius{};
		options.Flags = DependencyPropertyFlags::AffectsMeasure
			| DependencyPropertyFlags::AffectsRender;
		options.Validate = IsFiniteNonNegativeCornerRadius;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 200;
		options.Design.Order = 50;
		options.Design.Persistence =
			DependencyPropertyPersistence::Metadata;
		options.Design.Editor = DependencyPropertyEditorKind::Text;
		)
		return DependencyPropertyRegistry::RegisterStatic<
			Border, ::CornerRadius>(
				DependencyPropertyRegistrationLiteral(L"CornerRadius"),
				std::move(options));
	}();
	return *registration;
}

GET_CPP(Border, ::CornerRadius, CornerRadius)
{
	return GetDependencyPropertyValue<::CornerRadius>(
		CornerRadiusProperty());
}

SET_CPP(Border, ::CornerRadius, CornerRadius)
{
	(void)SetDependencyPropertyValue(
		CornerRadiusProperty(), std::move(value));
}

const DependencyProperty& Border::PaddingProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Border, Thickness> options;
		options.DefaultValue = Thickness{};
		options.Flags = DependencyPropertyFlags::AffectsMeasure
			| DependencyPropertyFlags::AffectsRender;
		options.Validate = IsFiniteNonNegativeThickness;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Layout";
		options.Design.CategoryOrder = 100;
		options.Design.Order = 80;
		options.Design.Persistence =
			DependencyPropertyPersistence::Native;
		options.Design.Editor =
			DependencyPropertyEditorKind::Thickness;
		)
		return DependencyPropertyRegistry::RegisterStatic<Border, Thickness>(
			DependencyPropertyRegistrationLiteral(L"Padding"),
			std::move(options));
	}();
	return *registration;
}

GET_CPP(Border, Thickness, Padding)
{
	return GetDependencyPropertyValue<Thickness>(PaddingProperty());
}

SET_CPP(Border, Thickness, Padding)
{
	(void)SetDependencyPropertyValue(PaddingProperty(), std::move(value));
}

void Border::RegisterDependencyProperties()
{
	Decorator::RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	(void)BorderBrushProperty();
	(void)BackgroundProperty();
	(void)BorderThicknessProperty();
	(void)CornerRadiusProperty();
	(void)PaddingProperty();
#endif
}

const DependencyPropertyMetadata*
Border::ResolveExactDependencyPropertyMetadata(
	const DependencyProperty& property) const
{
	if (&property == &Panel::BackgroundProperty())
		return &BackgroundPropertyMetadataRelation().Metadata();
	return Decorator::ResolveExactDependencyPropertyMetadata(property);
}

cui::core::Insets Border::GetDecoratorInsets() const noexcept
{
	const auto padding = const_cast<Border*>(this)->Padding;
	const auto borderThickness = const_cast<Border*>(this)->BorderThickness;
	return {
		padding.Left + borderThickness.Left,
		padding.Top + borderThickness.Top,
		padding.Right + borderThickness.Right,
		padding.Bottom + borderThickness.Bottom };
}

void Border::OnRender()
{
	if (!IsVisible || !GetPresentationWindow() || !GetDrawingContext()) return;
	auto& graphics = *GetDrawingContext();
	const auto size = GetActualSizeDip();
	const float width = size.width;
	const float height = size.height;
	const auto borderThickness = BorderThickness;
	const auto cornerRadius = CornerRadius;
	const D2D1_RECT_F outerRect =
		D2D1::RectF(0.0f, 0.0f, width, height);
	const D2D1_RECT_F innerRect = D2D1::RectF(
		(std::min)(borderThickness.Left, width),
		(std::min)(borderThickness.Top, height),
		(std::max)(0.0f, width - borderThickness.Right),
		(std::max)(0.0f, height - borderThickness.Bottom));
	const auto outerRadii =
		ResolveBorderRadii(cornerRadius, borderThickness, true);
	const auto innerRadii =
		ResolveBorderRadii(cornerRadius, borderThickness, false);

	BeginRender();
	if (auto* background = CreateBackgroundBrush(
		graphics, D2D1_SIZE_F{ width, height }))
	{
		if (innerRect.right > innerRect.left
			&& innerRect.bottom > innerRect.top)
		{
			if (auto geometry = CachedBorderGeometry(
				_backgroundGeometryCache,
				innerRect, innerRadii, nullptr, nullptr))
				graphics.FillGeometry(geometry.Get(), background);
		}
		background->Release();
	}
	if (auto* border = CreateBorderBrush(
		graphics, D2D1_SIZE_F{ width, height }))
	{
		if (auto geometry = CachedBorderGeometry(
			_borderGeometryCache,
			outerRect, outerRadii, &innerRect, &innerRadii))
			graphics.FillGeometry(geometry.Get(), border);
		border->Release();
	}
	EndRender();
}
