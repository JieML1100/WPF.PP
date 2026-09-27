#pragma once

#include "Control.h"

namespace cui::framework
{
	/** Narrow framework/test bridge for protected presentation lifecycle hooks. */
	struct PresentationAccess final
	{
		class RenderTransformSuppressionScope final
		{
		public:
			explicit RenderTransformSuppressionScope(Control& root) noexcept
				: _singleRoot{ &root },
				  _previous(Control::
					ExchangeRenderTransformSuppressionsForRecording(
						std::span<Control* const>{ _singleRoot }))
			{
			}

			explicit RenderTransformSuppressionScope(
				std::span<Control* const> roots) noexcept
				: _previous(Control::
					ExchangeRenderTransformSuppressionsForRecording(roots))
			{
			}

			RenderTransformSuppressionScope(
				const RenderTransformSuppressionScope&) = delete;
			RenderTransformSuppressionScope& operator=(
				const RenderTransformSuppressionScope&) = delete;

			~RenderTransformSuppressionScope() noexcept
			{
				(void)Control::
					ExchangeRenderTransformSuppressionsForRecording(_previous);
			}

		private:
			std::array<Control*, 1> _singleRoot{};
			std::span<Control* const> _previous{};
		};

		static void Prepare(Control& target)
		{
			target.PreparePresentation();
		}

		static void NotifyDpiChanged(Control& target, float dpiScale)
		{
			target.NotifyDpiChanged(dpiScale);
		}

		static void NotifyDeviceResourcesInvalidated(Control& target) noexcept
		{
			target.NotifyDeviceResourcesInvalidated();
		}

		static D2D1_MATRIX_3X2_F LocalToRenderTransformForRecording(
			const Control& target)
		{
			return target.GetLocalToRenderTransformForRecording();
		}

		static D2D1_RECT_F RenderedAbsoluteRectForRecording(
			const Control& target)
		{
			return target.GetRenderedAbsoluteRectDipForRecording();
		}

		static cui::core::Size RenderSize(Control& target)
		{
			return target.GetRenderSizeDip();
		}

		static bool BreaksVisualPresentationInheritance(
			const Control& target) noexcept
		{
			return target.BreaksVisualPresentationInheritance();
		}

		static bool AdvanceVisualStateAnimations(
			Control& target, unsigned long long nowMilliseconds)
		{
			return target.AdvanceVisualStateAnimations(nowMilliseconds);
		}

		/** Testing-only: exchanges this Control's declarative animation clock. */
		static std::optional<unsigned long long>
			ExchangeVisualStateAnimationClockOverrideForTesting(
				Control& target,
				std::optional<unsigned long long> value) noexcept
		{
			return target.ExchangeVisualStateAnimationClockOverrideForTesting(value);
		}

		/** Testing-only: reports a failed evaluation/commit, not clock inactivity. */
		static bool VisualStateAnimationAdvanceFailedForTesting(
			const Control& target) noexcept
		{
			return target.VisualStateAnimationAdvanceFailedForTesting();
		}

		/** Testing-only: rejects the next staged animation frame before DP commit. */
		static void FailNextVisualStateAnimationFrameCommitForTesting(
			Control& target) noexcept
		{
			target.FailNextVisualStateAnimationFrameCommitForTesting();
		}

		/** Testing-only: counts retained declarative animation leaves. */
		static size_t VisualStateAnimationLeafCountForTesting(
			const Control& target) noexcept
		{
			return target.VisualStateAnimationLeafCountForTesting();
		}

		/** Testing-only: counts distinct declarative root ClockId values. */
		static size_t VisualStateAnimationRootClockCountForTesting(
			const Control& target) noexcept
		{
			return target.VisualStateAnimationRootClockCountForTesting();
		}

		/** Testing-only synchronous aligned seek for a single retained root. */
		static bool SeekSingleVisualStateAnimationRootAlignedForTesting(
			Control& target, unsigned long long offsetMilliseconds)
		{
			return target.SeekSingleVisualStateAnimationRootAlignedForTesting(
				offsetMilliseconds);
		}

		/** Testing-only ordinary seek; commits on the next explicit tick. */
		static bool SeekSingleVisualStateAnimationRootForTesting(
			Control& target, unsigned long long offsetMilliseconds)
		{
			return target.SeekSingleVisualStateAnimationRootForTesting(
				offsetMilliseconds);
		}

		static bool PauseSingleVisualStateAnimationRootForTesting(Control& target)
		{
			return target.PauseSingleVisualStateAnimationRootForTesting();
		}

		static bool ResumeSingleVisualStateAnimationRootForTesting(Control& target)
		{
			return target.ResumeSingleVisualStateAnimationRootForTesting();
		}

		static bool SetSpeedRatioSingleVisualStateAnimationRootForTesting(
			Control& target, double ratio)
		{
			return target.SetSpeedRatioSingleVisualStateAnimationRootForTesting(
				ratio);
		}

		static bool SkipSingleVisualStateAnimationRootToFillForTesting(
			Control& target)
		{
			return target.SkipSingleVisualStateAnimationRootToFillForTesting();
		}

		static bool StopSingleVisualStateAnimationRootForTesting(Control& target)
		{
			return target.StopSingleVisualStateAnimationRootForTesting();
		}

		static bool RemoveSingleVisualStateAnimationRootForTesting(Control& target)
		{
			return target.RemoveSingleVisualStateAnimationRootForTesting();
		}

		static std::optional<DeclarativeClockObservation>
			QuerySingleVisualStateAnimationRootForTesting(
				const Control& target) noexcept
		{
			return target.QuerySingleVisualStateAnimationRootForTesting();
		}

		static size_t VisualStateAnimationClockNodeCountForTesting(
			const Control& target) noexcept
		{
			return target.VisualStateAnimationClockNodeCountForTesting();
		}

		static size_t VisualStateAnimationLayerStackCountForTesting(
			const Control& target) noexcept
		{
			return target.VisualStateAnimationLayerStackCountForTesting();
		}

		static size_t VisualStateAnimationLayerCountForTesting(
			const Control& target) noexcept
		{
			return target.VisualStateAnimationLayerCountForTesting();
		}

		static size_t VisualStateAnimationLayerMaxDepthForTesting(
			const Control& target) noexcept
		{
			return target.VisualStateAnimationLayerMaxDepthForTesting();
		}

		static size_t VisualStateAnimationRootClockChildCountForTesting(
			const Control& target,
			size_t rootIndex) noexcept
		{
			return target.VisualStateAnimationRootClockChildCountForTesting(
				rootIndex);
		}

		/** Testing-only: returns the sole root ClockId, or zero for none/multiple. */
		static uint64_t VisualStateAnimationSingleRootClockIdForTesting(
			const Control& target) noexcept
		{
			return target.VisualStateAnimationSingleRootClockIdForTesting();
		}

		static uint64_t VisualStateAnimationSingleRootClockNodeTokenForTesting(
			const Control& target) noexcept
		{
			return target.
				VisualStateAnimationSingleRootClockNodeTokenForTesting();
		}

		/** Testing-only validation of structured clock ownership. */
		static bool VisualStateAnimationClockIdentityValidForTesting(
			const Control& target) noexcept
		{
			return target.VisualStateAnimationClockIdentityValidForTesting();
		}

		/** Queues exactly one native animation leaf without advancing siblings. */
		static bool InvalidateNativeAnimationFrame(Control& target)
		{
			if (!target.IsAnimationRunning()) return false;
			D2D1_RECT_F rect{};
			if (target.GetAnimatedInvalidRect(rect))
				target.InvalidateVisualRect(rect);
			else
				target.InvalidateVisual();
			return true;
		}

		static void InvalidateMeasureSubtree(Control& target)
		{
			target.InvalidateMeasureSubtree();
		}

		static void InvalidateVisualRect(
			Control& target, const D2D1_RECT_F& contentRect)
		{
			target.InvalidateVisualRect(contentRect);
		}
	};
}
