#ifndef CUI_VISUAL_H_INCLUDED
#define CUI_VISUAL_H_INCLUDED
#pragma once

#include "DependencyObject.h"
#include "Geometry.h"
#include "ObservableCollection.h"
#include "Transform.h"

#include <d2d1.h>
#include <cstdint>
#include <span>
#include <vector>

class Control;
class Window;

namespace cui::framework
{
	struct TreeAccess;
}

/** Owned visual-child collection. Mutations are synchronized by Visual. */
class VisualCollection final : public ObservableCollection<Control*>
{
public:
	using CollectionBase = ObservableCollection<Control*>;
	VisualCollection() = default;
	VisualCollection(const VisualCollection&) = delete;
	VisualCollection(VisualCollection&&) = delete;
	VisualCollection& operator=(const VisualCollection&) = delete;
	VisualCollection& operator=(VisualCollection&&) = delete;
	VisualCollection& operator=(std::initializer_list<Control*>) = delete;

private:
	void RestoreRejectedMutation(std::span<Control* const> snapshot)
	{
		RestoreOwnerSnapshot(snapshot);
	}
	using CollectionBase::SetOwnerChangedHandler;
	using CollectionBase::SetOwnerSynchronizationDuringUpdates;
	friend class Control;
};

using VisualParentChangedEvent = Event<void(Control*, Control*, Control*)>;

/** How a visual contributes pixels to the retained presentation scene. */
enum class PresentationSurfaceKind : uint8_t
{
	Drawing,
	NativeComposition,
};

/** Independent retained-scene invalidation lanes for one Visual. */
enum class PresentationInvalidationKind : uint8_t
{
	None = 0,
	Content = 1u << 0,
	Geometry = 1u << 1,
	Composition = 1u << 2,
	/** Geometry moved by RenderTransform; isolated DComp content may be reused. */
	Transform = 1u << 3,
};

inline PresentationInvalidationKind operator|(
	PresentationInvalidationKind left,
	PresentationInvalidationKind right) noexcept
{
	return static_cast<PresentationInvalidationKind>(
		static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

inline bool HasPresentationInvalidation(
	PresentationInvalidationKind value,
	PresentationInvalidationKind lane) noexcept
{
	return (static_cast<uint8_t>(value) & static_cast<uint8_t>(lane)) != 0;
}

/** Monotonic local revisions consumed by PresentationScene nodes. */
struct PresentationRevisionSnapshot
{
	uint64_t Content = 1;
	uint64_t Geometry = 1;
	uint64_t Composition = 1;
};

/** CPU wall-time breakdown for the most recent successful presentation frame. */
struct PresentationFrameTimingStatistics
{
	double LayoutMicroseconds = 0.0;
	double SceneSynchronizationMicroseconds = 0.0;
	double CompositionPreparationMicroseconds = 0.0;
	double TransactionBeginMicroseconds = 0.0;
	double PrimarySetupMicroseconds = 0.0;
	double SceneRenderMicroseconds = 0.0;
	double SurfaceFinalizeMicroseconds = 0.0;
	double CompositionCommitMicroseconds = 0.0;
	double TotalMicroseconds = 0.0;
};

/** CPU work performed while resolving retained scene layers for one frame. */
struct PresentationPreparationStatistics
{
	double ScratchMicroseconds = 0.0;
	double NodePreparationMicroseconds = 0.0;
	double SegmentMicroseconds = 0.0;
	double RootStateMicroseconds = 0.0;
	double AncestorClipMicroseconds = 0.0;
	double BoundsMicroseconds = 0.0;
	double TransformClassificationMicroseconds = 0.0;
	double LayerAcquireStageMicroseconds = 0.0;
	double TopologyCommitMicroseconds = 0.0;
	double GroupStageMicroseconds = 0.0;
	size_t PreparedNodeCount = 0;
	size_t SegmentCount = 0;
	size_t PhysicalLayerRequiredCount = 0;
	size_t DeferredUnmaterializedCount = 0;
	size_t EarlyViewportDeferredCount = 0;
	/** Device-independent D2D mask geometries created during this preparation. */
	size_t AncestorGeometryMaskMaterializationCount = 0;
	/** Segment mask bindings satisfied by an already materialized geometry. */
	size_t AncestorGeometryMaskReuseCount = 0;
	/** Physical arbitrary-Geometry surfaces containing multiple sibling members. */
	size_t GeometryRasterGroupCount = 0;
	/** Logical sibling isolation members packed into those shared surfaces. */
	size_t GeometryRasterMemberCount = 0;
};

/** Classification of work performed by the most recent scene frame. */
struct PresentationFrameStatistics
{
	uint64_t Frame = 0;
	uint64_t Transaction = 0;
	uint64_t ResourceGeneration = 0;
	size_t ContentDirtyNodes = 0;
	size_t GeometryDirtyNodes = 0;
	size_t CompositionDirtyNodes = 0;
	size_t GeometryRecomputedNodes = 0;
	size_t CompositionTransformOnlyNodes = 0;
	size_t CompositionOnlySegments = 0;
	size_t SceneSurfacesOpened = 0;
	/** Arbitrary Geometry layer pushes submitted for retained scene surfaces. */
	size_t AncestorGeometryMaskLayerPushCount = 0;
	/** Raster subtree opacity groups submitted through Direct2D layers. */
	size_t OpacityLayerPushCount = 0;
	/** Shared arbitrary-Geometry surfaces updated through a strict partial rect. */
	size_t GeometryRasterPartialUpdateCount = 0;
	/** Shared arbitrary-Geometry surfaces conservatively redrawn in full. */
	size_t GeometryRasterFullReplayCount = 0;
	/** Nodes replayed because they intersect a shared Geometry damage rect. */
	size_t GeometryRasterPartialReplayNodes = 0;
	/** Clean nodes proven disjoint from a shared Geometry damage rect. */
	size_t GeometryRasterPartialSkippedNodes = 0;
	/** Total logical-pixel area submitted across shared Geometry partial updates. */
	uint64_t GeometryRasterPartialDamageArea = 0;
	double SceneSurfaceOpenMicroseconds = 0.0;
	double SceneSurfaceCloseMicroseconds = 0.0;
	double SceneSurfaceEndDrawMicroseconds = 0.0;
	double SceneSurfacePresentMicroseconds = 0.0;
	double SceneSurfaceSubmitMicroseconds = 0.0;
	double SceneCommandRecordMicroseconds = 0.0;
	double SceneCommandReplayMicroseconds = 0.0;
	size_t ImmediateDrawNodes = 0;
	size_t DamageReplayNodes = 0;
	size_t CommandRecordedNodes = 0;
	size_t CommandReplayedNodes = 0;
	size_t CommandCacheHitNodes = 0;
	/** Cache hits replayed with a rigid translation instead of re-recording. */
	size_t CommandTranslatedNodes = 0;
	/** Command lists carried across a scene topology rebuild. */
	size_t CommandCacheRetainedNodes = 0;
	size_t CommandCacheInvalidatedNodes = 0;
	size_t NativeCommitNodes = 0;
	size_t CulledNodes = 0;
	/** Nodes rejected before opening their retained scene swap chain. */
	size_t PreSurfaceCulledNodes = 0;
	PresentationPreparationStatistics Preparation;
	PresentationFrameTimingStatistics Timing;
};

/** Owns the retained visual relation, transform, clip and dirty-region state. */
class Visual : public DependencyObject
{
private:
	VisualCollection _visualChildren;
	friend class Control;
	friend struct cui::framework::TreeAccess;

protected:
	using PresentationWindow = Window;

	PresentationWindow* _presentationWindow = nullptr;
	void SetPresentationWindowCore(PresentationWindow* value) noexcept
	{
		_presentationWindow = value;
	}
	std::vector<Control*> _observedVisualChildren;
	Control* _visualParent = nullptr;
	int _zIndex = 0;
	D2D1_RECT_F _lastInvalidatedClientRect{ 0, 0, 0, 0 };
	bool _hasLastInvalidatedClientRect = false;
	bool _hasPresentationOrderOverride = false;
	int _presentationOrderOverride = 0;
	bool _isWindowRoot = false;
	std::optional<cui::drawing::Geometry> _clip;
	std::optional<cui::drawing::Transform> _renderTransform;
	D2D1_POINT_2F _renderTransformOrigin{ 0.0f, 0.0f };
	size_t _activeGeometryClipCount = 0;
	/** Bit i set: ancestor clip i was pushed as an axis-aligned clip. */
	uint64_t _activeAxisAlignedClipMask = 0;
	PresentationRevisionSnapshot _presentationRevisions;
	VisualParentChangedEvent OnVisualParentChanged;

public:
	Visual() = default;
	~Visual() override = default;

	/** Read-only projection of the native presentation host; tree attachment owns mutation. */
	PresentationWindow* GetPresentationWindow() const noexcept
	{
		return _presentationWindow;
	}
	/** Read-only visual-tree projection; mutations must use Control ownership APIs. */
	std::span<Control* const> GetVisualChildrenView() const noexcept
	{
		return { _visualChildren.data(), _visualChildren.size() };
	}
	__declspec(property(put = SetZIndex, get = GetZIndex)) int ZIndex;
	int GetZIndex();
	void SetZIndex(int value);

	Control* GetVisualParent() const noexcept { return _visualParent; }
	PresentationRevisionSnapshot GetPresentationRevisions() const noexcept
	{
		return _presentationRevisions;
	}
	virtual void InvalidateVisual() = 0;
};

#endif // CUI_VISUAL_H_INCLUDED
