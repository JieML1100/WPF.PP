#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include <wrl/client.h>

#include "PresentationRenderHost.h"
#include "Geometry.h"
#include "Visual.h"
#include "ControlWeakReference.h"

class Control;
class D2DGraphics;
struct ID2D1CommandList;

/** Shared DComp layer used by both retained drawing segments and native visuals. */
inline constexpr int PresentationSceneContentLayer = 1000;
/** Global DComp layer reserved for independently segmented transient roots. */
inline constexpr int PresentationSceneOverlayLayer = 200000;

/** Read-only retained-node snapshot used by framework diagnostics and tests. */
struct PresentationNodeSnapshot final
{
	bool ContentDirty = false;
	bool GeometryDirty = false;
	bool CompositionDirty = false;
	bool HasGeometry = false;
	bool HasPresented = false;
	bool HasDrawingCommands = false;
	bool NativeComposition = false;
	bool Overlay = false;
	bool CompositionIsolated = false;
	bool CompositionIsolationRoot = false;
	size_t CompositionIsolationDepth = 0;
	bool TransformOnlyGeometryDirty = false;
	size_t SegmentIndex = static_cast<size_t>(-1);
	D2D1_MATRIX_3X2_F CompositionTransform{
		1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
	D2D1_POINT_2F CompositionSurfaceOriginDip{};
	UINT CompositionSurfacePhysicalWidth = 0;
	UINT CompositionSurfacePhysicalHeight = 0;
	bool CompositionHasAncestorClip = false;
	float CompositionOpacity = 1.0f;
	D2D1_RECT_F RenderedBounds{};
};

/**
 * Retained, derived presentation snapshot for one Window visual tree.
 *
 * The snapshot owns no Controls and carries no document identity. Structural
 * mutations invalidate its topology; content, geometry and composition use
 * independent node revisions. Rendered bounds are cached per geometry revision,
 * while animated ancestor clips and damage remain live frame inputs. This keeps
 * tree discovery and D2D/native segmentation outside the native Window shell.
 */
class PresentationScene final
{
public:
	PresentationScene() = default;
	PresentationScene(const PresentationScene&) = delete;
	PresentationScene& operator=(const PresentationScene&) = delete;

	void InvalidateStructure() noexcept { _structureDirty = true; }
	void InvalidateNode(
		Control* control,
		PresentationInvalidationKind kind) noexcept;
	bool Synchronize(
		std::span<Control* const> roots,
		std::span<Control* const> transientRoots,
		std::span<Control* const> compositionIsolationTargets,
		std::span<Control* const> opacityIsolationTargets = {});
	/** Drops all device-domain caches when the host generation changes. */
	void SynchronizeResourceGeneration(uint64_t generation) noexcept;

	bool RequiresComposition() const noexcept { return _requiresComposition; }
	bool IsRendering() const noexcept { return _rendering; }
	uint64_t Revision() const noexcept { return _revision; }
	uint64_t ContentRevision() const noexcept { return _contentRevision; }
	uint64_t GeometryRevision() const noexcept { return _geometryRevision; }
	uint64_t CompositionRevision() const noexcept
	{
		return _compositionRevision;
	}
	size_t NodeCount() const noexcept { return _nodes.size(); }
	size_t DrawingLayerCount() const noexcept { return _segments.size(); }
	size_t OpacityGroupCount() const noexcept { return _opacityGroups.size(); }
	PresentationFrameStatistics FrameStatistics() const noexcept
	{
		return _frameStatistics;
	}
	void SetFrameTimingStatistics(
		const PresentationFrameTimingStatistics& value) noexcept
	{
		_frameStatistics.Timing = value;
	}
	bool TryGetNodeSnapshot(
		const Control* control,
		PresentationNodeSnapshot& out) const noexcept;

	/** Resolves every logical segment and materializes only required D2D layers. */
	bool PrepareComposition(
		PresentationRenderHost& host,
		int titleBarOffsetDip,
		float dpiScale);
	/** Draws the flattened scene into retained D2D layers and native visuals. */
	bool RenderComposition(
		PresentationRenderHost& host,
		PresentationRenderHost::FrameTransaction& transaction,
		const RECT& contentDirty,
		int titleBarOffset,
		float logicalWidth,
		float logicalHeight);
	/** Draws flattened content nodes into the active raster surface. */
	bool RenderRaster(const RECT& contentDirty);
	/** Returns whether the independent transparent overlay needs a new frame. */
	bool RequiresOverlayFrame();
	/** Draws flattened transient nodes into the active overlay/raster surface. */
	bool RenderOverlay(
		const RECT& contentDirty,
		bool includeCompositionSegments = false);

	int GetOrder(const Control* control) const noexcept;

private:
	static constexpr size_t NoSegment = static_cast<size_t>(-1);

	struct Node
	{
		// The retained scene is allowed to be invalidated by a control while an
		// earlier node is preparing or rendering.  Keep only a lifetime-checked
		// identity here: a raw Control* can otherwise outlive a visual mutation
		// until the next frame synchronizes the topology.
		ControlWeakReference Element;
		int Order = 0;
		size_t SegmentIndex = NoSegment;
		/** Member inside an exact shared arbitrary-Geometry raster surface. */
		size_t GeometryRasterMemberIndex = NoSegment;
		size_t SubtreeEnd = 0;
		bool NativeComposition = false;
		bool Overlay = false;
		/** True when the complete transient-root set owns independent DComp layers. */
		bool OverlaySegmented = false;
		bool CompositionIsolated = false;
		bool CompositionIsolationRoot = false;
		bool TransformOnlyGeometryDirty = false;
		bool ContentDirty = true;
		bool GeometryDirty = true;
		bool CompositionDirty = true;
		bool HasGeometry = false;
		bool HasPresented = false;
		PresentationRevisionSnapshot AppliedRevisions{};
		D2D1_RECT_F RenderedBounds{};
		Microsoft::WRL::ComPtr<ID2D1CommandList> DrawingCommands;
		uint64_t CommandGeneration = 0;
		/**
		 * Rigid-translation reuse of DrawingCommands for ordinary (non-isolated)
		 * nodes. A node qualifies when no ancestor clip touched its pixels at
		 * recording time; while that remains true and only the translation of
		 * its local-to-render transform changes (scrolling, virtualized row
		 * rebasing), the retained list is replayed displaced by ReplayOffset
		 * instead of calling OnRender again.
		 */
		bool TranslationReplayable = false;
		D2D1_MATRIX_3X2_F RecordedTransform{
			1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
		D2D1_SIZE_F RecordedRenderSize{};
		D2D1_SIZE_F RecordedActualSize{};
		int RecordedTitleBarOffset = 0;
		D2D1_POINT_2F ReplayOffset{};
		/** Visual parent at topology build time; compared only, never dereferenced. */
		const Control* VisualParent = nullptr;
	};

	/**
	 * Command list and revision snapshot of an ordinary node carried across a
	 * topology rebuild. Dormant entries belong to nodes that left the scene (a
	 * hidden TabControl page, a collapsed panel) and are adopted with forced
	 * geometry validation when the same control re-enters under the same parent.
	 */
	struct RetainedCommandCache
	{
		ControlWeakReference Element;
		const Control* VisualParent = nullptr;
		bool Dormant = false;
		Microsoft::WRL::ComPtr<ID2D1CommandList> DrawingCommands;
		uint64_t CommandGeneration = 0;
		PresentationRevisionSnapshot AppliedRevisions{};
		D2D1_RECT_F RenderedBounds{};
		bool HasGeometry = false;
		bool ContentDirty = true;
		bool GeometryDirty = true;
		bool CompositionDirty = true;
		bool TranslationReplayable = false;
		D2D1_MATRIX_3X2_F RecordedTransform{
			1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
		D2D1_SIZE_F RecordedRenderSize{};
		D2D1_SIZE_F RecordedActualSize{};
		int RecordedTitleBarOffset = 0;
		D2D1_POINT_2F ReplayOffset{};
	};
	static constexpr size_t MaxDormantCommandLists = 32768;

	struct Segment
	{
		struct GeometryRasterMember
		{
			ControlWeakReference IsolationRoot;
			std::vector<ControlWeakReference> IsolationRoots;
			size_t NodeStart = 0;
			size_t NodeEnd = 0;
			D2D1_MATRIX_3X2_F LogicalTransform{
				1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
			D2D1_MATRIX_3X2_F LogicalInverse{
				1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
			bool HasLogicalInverse = true;
		};
		struct AncestorGeometryClip
		{
			cui::drawing::Geometry Value;
			D2D1_MATRIX_3X2_F LocalToRootDip{
				1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
			D2D1_MATRIX_3X2_F NativeTransformDip{
				1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
			Microsoft::WRL::ComPtr<ID2D1Geometry> NativeGeometry;
		};
		struct AncestorClip
		{
			D2D1_RECT_F LocalRectDip{};
			float RadiusXDip = 0.0f;
			float RadiusYDip = 0.0f;
			D2D1_MATRIX_3X2_F LocalToRootDip{
				1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
		};
		int Order = 0;
		int Layer = PresentationSceneContentLayer;
		D2DGraphics* Context = nullptr;
		ControlWeakReference IsolationRoot;
		std::vector<ControlWeakReference> IsolationRoots;
		std::vector<ControlWeakReference> OpacityIsolationRoots;
		std::vector<ControlWeakReference> OpacityGroupRoots;
		size_t NodeStart = 0;
		size_t NodeEnd = 0;
		D2D1_POINT_2F SurfaceOriginDip{};
		RECT LogicalSurfaceClient{ 0, 0, 1, 1 };
		PresentationRenderHost::SceneLayerSurfaceProperties SurfaceProperties;
		D2D1_MATRIX_3X2_F LogicalTransform{
			1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
		D2D1_MATRIX_3X2_F LogicalInverse{
			1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
		float StagedOpacity = 1.0f;
		D2D1_RECT_F AncestorClipRootDip{};
		std::vector<AncestorClip> AncestorClipChain;
		std::vector<AncestorGeometryClip> AncestorGeometryClips;
		/** Two or more exact sibling isolation ranges sharing one D2D mask/surface. */
		std::vector<GeometryRasterMember> GeometryRasterMembers;
		bool HasAncestorClip = false;
		bool RasterizesAncestorGeometryClip = false;
		bool HasLogicalInverse = true;
	};

	struct OpacityGroup
	{
		struct NativeMember final
		{
			ControlWeakReference Root;
			int Order = 0;
		};
		ControlWeakReference Root;
		size_t FirstSegment = 0;
		size_t SegmentCount = 0;
		size_t ParentGroup = NoSegment;
		int Order = 0;
		float StagedOpacity = 1.0f;
		std::vector<NativeMember> NativeMembers;
		int Layer = PresentationSceneContentLayer;
	};

	std::vector<Control*> _rasterRoots;
	std::vector<Node> _nodes;
	std::vector<Segment> _segments;
	std::vector<OpacityGroup> _opacityGroups;
	std::unordered_map<Control*, size_t> _nodeIndex;
	struct PendingGeometryRange
	{
		size_t Start = 0;
		size_t End = 0;
		bool TransformOnly = false;
	};
	std::vector<PendingGeometryRange> _pendingGeometryRanges;
	/** Reused frame-local classification storage; stable topology allocates once. */
	std::vector<uint8_t> _segmentSubmissionScratch;
	std::vector<uint8_t> _preparedNodeScratch;
	/** Frame-local surface-space damage for exact shared Geometry groups. */
	std::vector<RECT> _geometryRasterDamageScratch;
	/** Frame-local current root-space bounds used for damage intersection. */
	std::vector<D2D1_RECT_F> _geometryRasterBoundsScratch;
	std::vector<uint8_t> _geometryRasterBoundsValidScratch;
	std::vector<uint8_t> _geometryRasterPartialScratch;
	std::vector<PresentationRenderHost::SceneLayerGroupProperties>
		_groupPropertiesScratch;
	bool _allGeometryDirty = false;
	std::vector<Control*> _transientRoots;
	std::vector<ControlWeakReference> _compositionIsolationTargets;
	std::vector<ControlWeakReference> _opacityIsolationTargets;
	bool _requiresComposition = false;
	bool _overlaySurfaceDirty = true;
	bool _structureDirty = true;
	bool _rendering = false;
	uint64_t _revision = 0;
	uint64_t _contentRevision = 1;
	uint64_t _geometryRevision = 1;
	uint64_t _compositionRevision = 1;
	uint64_t _frameSequence = 0;
	uint64_t _resourceGeneration = 0;
	size_t _pendingCommandCacheInvalidations = 0;
	size_t _pendingCommandCacheRetentions = 0;
	std::unordered_map<const Control*, RetainedCommandCache> _dormantCommands;
	uint64_t _preparedRevision = 0;
	bool _hasPreparedRevision = false;
	PresentationFrameStatistics _frameStatistics;
	PresentationPreparationStatistics _preparedStatistics;

	void Rebuild(std::span<Control* const> roots);
	bool RefreshNodeState(Node& node);
	void ApplyPendingGeometryInvalidations();
	bool RefreshNodeGeometry(Node& node);
	bool CompleteNode(
		Node& node,
		const PresentationRevisionSnapshot& submitted);
	bool PrepareNodeForRendering(Node& node, Control*& control);
	void BeginFrameStatistics() noexcept;
};
