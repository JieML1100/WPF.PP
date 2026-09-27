#pragma once

#include <Windows.h>
#include <d2d1.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

class D2DGraphics;
class DCompLayeredHost;
struct ID2D1CommandList;
struct IDCompositionDevice;
struct IDCompositionEffectGroup;
struct IDCompositionRectangleClip;
struct IDCompositionVisual;

/**
 * Owns every device-dependent resource for one native presentation source.
 *
 * A frame is an explicit transaction. Primary, retained scene and overlay
 * surfaces may only be opened through that transaction, and DirectComposition
 * is committed only after every surface closes successfully. Any EndDraw,
 * Present, DComp, exception or injected-device failure aborts the whole frame,
 * advances the device-resource generation during recovery and forces a full
 * retained resubmission.
 */
class PresentationRenderHost final
{
public:
	enum class SurfaceRole : uint8_t
	{
		Primary,
		Scene,
		Overlay
	};

	struct SurfaceFrame
	{
		D2DGraphics* Context = nullptr;
		SurfaceRole Role = SurfaceRole::Primary;
		RECT LogicalDirty{};
		double EndDrawMicroseconds = 0.0;
		double PresentMicroseconds = 0.0;
		double SurfaceSubmitMicroseconds = 0.0;
		bool Open = false;
	};

	struct FrameTransaction
	{
		uint64_t Sequence = 0;
		uint64_t ResourceGeneration = 0;
		RECT LogicalClient{};
		RECT LogicalDirty{};
		float DpiScale = 1.0f;
		bool FullFrame = false;
		bool Open = false;
		bool Failed = false;
		SurfaceFrame Primary;
	};

	struct TransactionStatistics
	{
		uint64_t LastSequence = 0;
		uint64_t ResourceGeneration = 0;
		uint64_t CommittedFrames = 0;
		uint64_t AbortedFrames = 0;
		uint64_t DeviceRecoveries = 0;
		uint64_t InjectedDeviceLosses = 0;
		uint64_t LastSurfaceFailureSequence = 0;
		SurfaceRole LastFailedSurfaceRole = SurfaceRole::Primary;
		HRESULT LastFailedEndDrawHr = S_OK;
		HRESULT LastFailedPresentHr = S_OK;
	};

	/** Read-only adapter and retained-surface resource telemetry. */
	struct ResourceSnapshot
	{
		uint64_t DeviceGeneration = 0;
		bool IsHardwareAdapter = false;
		bool IsSoftwareAdapter = false;
		bool SupportsVideo = false;
		uint32_t FeatureLevel = 0;
		uint32_t VendorId = 0;
		uint32_t DeviceId = 0;
		uint64_t AdapterLuid = 0;
		uint64_t DedicatedVideoMemoryBytes = 0;
		uint64_t SharedSystemMemoryBytes = 0;
		wchar_t AdapterDescription[128]{};
		uint64_t LocalMemoryBudgetBytes = 0;
		uint64_t LocalMemoryCurrentUsageBytes = 0;
		uint64_t NonLocalMemoryBudgetBytes = 0;
		uint64_t NonLocalMemoryCurrentUsageBytes = 0;
		/** Materialized DirectComposition scene layers. */
		size_t SceneLayerCount = 0;
		size_t SceneLayerSwapChainCount = 0;
		size_t SceneLayerCompositionSurfaceCount = 0;
		size_t SceneLayerDirect2DSurfaceContextCount = 0;
		size_t SceneLayerSubmittedSnapshotTextureCount = 0;
		size_t SceneLayerPixelReadbackLeaseCount = 0;
		/** Stable logical slots addressed by PresentationScene SegmentIndex. */
		size_t SceneLayerSlotCount = 0;
		size_t SceneLayerSlotCapacity = 0;
		size_t SceneLayerGroupCount = 0;
		size_t FullWindowSceneLayerCount = 0;
		size_t SceneLayerDistinctGraphicsDeviceCount = 0;
		size_t SceneLayerDistinctRecorderDeviceCount = 0;
		size_t SceneLayerSharedGraphicsDeviceCount = 0;
		size_t SceneLayerSharedRecorderDeviceCount = 0;
		size_t SceneCommandRecorderCount = 0;
		size_t SceneCommandRecorderReferenceCount = 0;
		UINT MaximumSceneSurfaceWidth = 0;
		UINT MaximumSceneSurfaceHeight = 0;
		uint64_t EstimatedSceneSwapChainBytes = 0;
		uint64_t EstimatedSceneCompositionSurfaceBytes = 0;
		uint64_t EstimatedSceneSubmittedSnapshotBytes = 0;
		uint64_t EstimatedSceneLayerSlotBytes = 0;
		uint64_t EstimatedSceneRetainedBytes = 0;
		uint64_t EstimatedPrimarySwapChainBytes = 0;
		uint64_t EstimatedOverlaySwapChainBytes = 0;
		uint64_t EstimatedTotalSwapChainBytes = 0;
		UINT PrimaryPresentSyncInterval = 0;
		UINT OverlayPresentSyncInterval = 0;
		UINT MinimumScenePresentSyncInterval = 0;
		UINT MaximumScenePresentSyncInterval = 0;
		size_t PeakSceneLayerCount = 0;
		size_t PeakSceneLayerSlotCount = 0;
		uint64_t PeakEstimatedSceneSwapChainBytes = 0;
		uint64_t PeakEstimatedSceneCompositionSurfaceBytes = 0;
		uint64_t PeakEstimatedSceneSubmittedSnapshotBytes = 0;
		uint64_t PeakEstimatedSceneLayerSlotBytes = 0;
		uint64_t SceneLayerCreateCount = 0;
		uint64_t SceneLayerResizeCount = 0;
		uint64_t SceneLayerReleaseCount = 0;
		uint64_t SceneLayerAllocationFailureCount = 0;
		uint64_t SceneLayerSubmittedSnapshotCreateCount = 0;
		uint64_t SceneLayerSubmittedSnapshotUpdateCount = 0;
		uint64_t SceneLayerSubmittedSnapshotCopiedBytes = 0;
		double SceneLayerSubmittedSnapshotCreateMicroseconds = 0.0;
		double SceneLayerSubmittedSnapshotCopyMicroseconds = 0.0;
		double SceneLayerSlotEnsureMicroseconds = 0.0;
		double SceneLayerTopologyBatchBeginMicroseconds = 0.0;
		double SceneLayerSwapChainCreateMicroseconds = 0.0;
		double SceneLayerCompositionSurfaceCreateMicroseconds = 0.0;
		double SceneLayerVisualCreateMicroseconds = 0.0;
		double SceneLayerVisualBindMicroseconds = 0.0;
		double SceneLayerGraphicsCreateMicroseconds = 0.0;
		double SceneLayerRecorderCreateMicroseconds = 0.0;
		double SceneLayerDpiSetupMicroseconds = 0.0;
		double SceneLayerVisualPropertyStageMicroseconds = 0.0;
		double SceneLayerGroupStageMicroseconds = 0.0;
		double SceneLayerResourcePeakUpdateMicroseconds = 0.0;
		double SceneLayerTopologyBatchCommitMicroseconds = 0.0;
		uint64_t CompositionVisualStackRebuildCount = 0;
		uint64_t CompositionVisualStackRebuildEntryCount = 0;
		uint64_t CompositionVisualDeferredMutationCount = 0;
		uint64_t CompositionVisualBatchCommitCount = 0;
		uint64_t CompositionVisualBatchRollbackCount = 0;
		uint64_t CompositionVisualBatchRollbackFailureCount = 0;
	};

	/**
	 * Physical-pixel properties staged on one retained DirectComposition layer.
	 * This is an internal backend primitive, not a second animation clock: the
	 * UI thread remains responsible for resolving every declarative DP value.
	 */
	struct SceneLayerTransformedClip
	{
		D2D1_RECT_F PhysicalClip{};
		float RadiusX = 0.0f;
		float RadiusY = 0.0f;
		/** Maps this clip's local physical-pixel coordinates into layer-root pixels. */
		D2D1_MATRIX_3X2_F LocalToRootPhysical{
			1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };

		bool operator==(const SceneLayerTransformedClip& other) const noexcept
		{
			return PhysicalClip.left == other.PhysicalClip.left
				&& PhysicalClip.top == other.PhysicalClip.top
				&& PhysicalClip.right == other.PhysicalClip.right
				&& PhysicalClip.bottom == other.PhysicalClip.bottom
				&& RadiusX == other.RadiusX
				&& RadiusY == other.RadiusY
				&& LocalToRootPhysical._11 == other.LocalToRootPhysical._11
				&& LocalToRootPhysical._12 == other.LocalToRootPhysical._12
				&& LocalToRootPhysical._21 == other.LocalToRootPhysical._21
				&& LocalToRootPhysical._22 == other.LocalToRootPhysical._22
				&& LocalToRootPhysical._31 == other.LocalToRootPhysical._31
				&& LocalToRootPhysical._32 == other.LocalToRootPhysical._32;
		}
	};

	struct SceneLayerVisualProperties
	{
		D2D1_MATRIX_3X2_F PhysicalTransform{
			1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
		float Opacity = 1.0f;
		D2D1_RECT_F PhysicalClip{};
		bool HasClip = false;
		/** Outer-to-inner ancestor-local rectangle clips. */
		std::vector<SceneLayerTransformedClip> TransformedClipChain;

		bool operator==(const SceneLayerVisualProperties& other) const noexcept
		{
			return PhysicalTransform._11 == other.PhysicalTransform._11
				&& PhysicalTransform._12 == other.PhysicalTransform._12
				&& PhysicalTransform._21 == other.PhysicalTransform._21
				&& PhysicalTransform._22 == other.PhysicalTransform._22
				&& PhysicalTransform._31 == other.PhysicalTransform._31
				&& PhysicalTransform._32 == other.PhysicalTransform._32
				&& Opacity == other.Opacity
				&& PhysicalClip.left == other.PhysicalClip.left
				&& PhysicalClip.top == other.PhysicalClip.top
				&& PhysicalClip.right == other.PhysicalClip.right
				&& PhysicalClip.bottom == other.PhysicalClip.bottom
				&& HasClip == other.HasClip
				&& TransformedClipChain == other.TransformedClipChain;
		}
	};

	/** Immutable-for-a-frame swap-chain geometry for one retained scene layer. */
	struct SceneLayerSurfaceProperties
	{
		UINT PhysicalWidth = 1;
		UINT PhysicalHeight = 1;
		bool FullWindow = true;

		bool operator==(const SceneLayerSurfaceProperties& other) const noexcept
		{
			return PhysicalWidth == other.PhysicalWidth
				&& PhysicalHeight == other.PhysicalHeight
				&& FullWindow == other.FullWindow;
		}
	};

	/**
	 * One post-composition opacity group over a contiguous scene-layer range.
	 * Child layers retain root-pixel transforms; the group owns the shared effect,
	 * any common rectangle-family clip chain, and the single global z-order slot
	 * for the complete subtree.
	 */
	struct SceneLayerGroupProperties
	{
		struct NativeVisual final
		{
			IDCompositionVisual* Visual = nullptr;
			int Order = 0;

			bool operator==(const NativeVisual& other) const noexcept
			{
				return Visual == other.Visual && Order == other.Order;
			}
		};

		static constexpr size_t NoParent = static_cast<size_t>(-1);
		size_t FirstLayer = 0;
		size_t LayerCount = 0;
		size_t ParentGroup = NoParent;
		float Opacity = 1.0f;
		int Layer = 0;
		int Order = 0;
		D2D1_RECT_F PhysicalClip{};
		bool HasClip = false;
		/** Outer-to-inner clip chain shared by the complete opacity group. */
		std::vector<SceneLayerTransformedClip> TransformedClipChain;
		/** Direct native-composition children ordered with direct scene layers. */
		std::vector<NativeVisual> NativeVisuals;
		bool HasSameTopology(
			const SceneLayerGroupProperties& other) const noexcept
		{
			return FirstLayer == other.FirstLayer
				&& LayerCount == other.LayerCount
				&& ParentGroup == other.ParentGroup
				&& Layer == other.Layer
				&& Order == other.Order
				&& HasClip == other.HasClip
				&& TransformedClipChain.size()
					== other.TransformedClipChain.size()
				&& NativeVisuals == other.NativeVisuals;
		}
	};

	PresentationRenderHost();
	~PresentationRenderHost();
	PresentationRenderHost(const PresentationRenderHost&) = delete;
	PresentationRenderHost& operator=(const PresentationRenderHost&) = delete;
	PresentationRenderHost(PresentationRenderHost&&) = delete;
	PresentationRenderHost& operator=(PresentationRenderHost&&) = delete;

	bool Attach(HWND window, UINT dpi = 96);
	void Detach() noexcept;
	bool IsAttached() const noexcept;
	/** One-shot test seam for the real transient initial-attach recovery path. */
	static void FailNextPrimaryAttachForTesting() noexcept;

	D2DGraphics* DrawingContext() const noexcept { return _active; }
	D2DGraphics* PrimaryContext() const noexcept { return _primary.get(); }
	D2DGraphics* OverlayContext() const noexcept { return _overlay.get(); }
	bool Activate(D2DGraphics* context) noexcept;

	void Resize(UINT width, UINT height);
	void ResizeToClient();
	void SetDpi(UINT dpi);
	UINT Dpi() const noexcept { return _dpi; }
	UINT PhysicalWidth() const noexcept { return _width; }
	UINT PhysicalHeight() const noexcept { return _height; }

	bool BeginFrameTransaction(
		const RECT& physicalDirty,
		bool force,
		FrameTransaction& transaction);
	bool OpenSceneSurface(
		FrameTransaction& transaction,
		D2DGraphics* context,
		const RECT& logicalDirty,
		const RECT& logicalSurfaceClient,
		SurfaceFrame& surface);
	bool OpenOverlaySurface(
		FrameTransaction& transaction,
		SurfaceFrame& surface);
	bool CloseSurface(
		FrameTransaction& transaction,
		SurfaceFrame& surface) noexcept;
	bool RecordDrawingCommands(
		FrameTransaction& transaction,
		D2DGraphics* presentationContext,
		const std::function<void()>& draw,
		ID2D1CommandList** commandList) noexcept;
	bool ReplayDrawingCommands(
		FrameTransaction& transaction,
		D2DGraphics* presentationContext,
		ID2D1CommandList* commandList,
		D2D1_POINT_2F offset = {}) noexcept;
	bool CommitFrameTransaction(FrameTransaction& transaction) noexcept;
	void AbortFrameTransaction(FrameTransaction& transaction) noexcept;
	bool IsTransactionActive(const FrameTransaction& transaction) const noexcept;

	/**
	 * Coalesces physical-client damage and schedules the matching HWND update
	 * region. The retained queue owns what to repaint; WM_PAINT owns when.
	 */
	void QueueDamage(const RECT& physicalDirty) noexcept;
	/** Promotes all pending damage to the complete physical client. */
	void QueueFullDamage() noexcept;
	/** Atomically transfers the currently coalesced physical-client damage. */
	bool TakePendingDamage(RECT& physicalDirty) noexcept;
	bool HasPendingDamage() const noexcept { return _hasPendingDamage; }
	bool TryGetLastPrimaryFrame(
		RECT& logicalDirty,
		bool& fullFrame) const noexcept;
	bool NeedsFullFrame(bool force = false) const noexcept
	{
		return force || !_hasPresentedFrame;
	}
	void InvalidateFrameHistory() noexcept;

	bool IsDeviceLost() const noexcept;
	bool RecoverDevice();
	/** Test seam: recovery follows the exact production generation path. */
	void InjectDeviceLossForTesting() noexcept;
	uint64_t ResourceGeneration() const noexcept
	{
		return _resourceGeneration;
	}
	/** Changes only when control-owned graphics/composition devices must rebind. */
	uint64_t DeviceResourceGeneration() const noexcept
	{
		return _deviceResourceGeneration;
	}
	TransactionStatistics Statistics() const noexcept;
	ResourceSnapshot Resources() const noexcept;

	bool EnsureComposition();
	bool UsesComposition() const noexcept;
	/** Number of slots that currently own a physical DComp layer. */
	size_t SceneLayerCount() const noexcept;
	/** Number of stable logical slots, including unmaterialized offscreen slots. */
	size_t SceneLayerSlotCount() const noexcept { return _sceneLayers.size(); }
	bool EnsureSceneLayerSlots(size_t count) noexcept;
	bool IsSceneLayerMaterialized(size_t index) const noexcept;
	bool BeginSceneLayerTopologyBatch() noexcept;
	bool CommitSceneLayerTopologyBatch() noexcept;
	void RollbackSceneLayerTopologyBatch() noexcept;
	/** Acquires a legacy full-window retained scene layer. */
	D2DGraphics* AcquireSceneLayer(size_t index, int layer, int order);
	D2DGraphics* AcquireSceneLayer(
		size_t index,
		int layer,
		int order,
		const SceneLayerSurfaceProperties& surfaceProperties);
	/** Stages validated properties; the next composition commit publishes them. */
	bool StageSceneLayerVisualProperties(
		size_t index,
		const SceneLayerVisualProperties& properties) noexcept;
	/** Reparents disjoint contiguous layer ranges under shared opacity visuals. */
	bool StageSceneLayerGroups(
		std::span<const SceneLayerGroupProperties> groups) noexcept;
	size_t SceneLayerGroupCountForTesting() const noexcept
	{
		return _sceneLayerGroups.size();
	}
	size_t SceneLayerGroupedNativeVisualCountForTesting() const noexcept;
	/** Internal/test observation of the last successfully staged values. */
	bool TryGetSceneLayerVisualProperties(
		size_t index,
		SceneLayerVisualProperties& properties) const noexcept;
	/** Internal/test observation of the retained swap-chain geometry. */
	bool TryGetSceneLayerSurfaceProperties(
		size_t index,
		SceneLayerSurfaceProperties& properties) const noexcept;
	/** Testing-only digest of the most recently presented scene-layer buffer. */
	bool TryGetSceneLayerPixelDigestForTesting(
		size_t index,
		UINT& width,
		UINT& height,
		uint64_t& digest,
		size_t& nonTransparentPixels) const noexcept;
	/**
	 * Enables exact GPU submitted-pixel capture for tests/readback clients.
	 *
	 * The first lease must be acquired before a composition surface's first
	 * complete draw. Production rendering holds no lease and therefore owns no
	 * submitted-snapshot texture or copy work.
	 */
	bool AcquireSceneLayerPixelReadbackLeaseForTesting() noexcept;
	void ReleaseSceneLayerPixelReadbackLeaseForTesting() noexcept;
	void TrimSceneLayers(size_t usedCount) noexcept;
	void ReleaseSceneLayers() noexcept;
	/** Fails the scene-layer allocation after N successful creates. */
	static void FailSceneLayerAllocationAfterForTesting(
		size_t successfulCreates) noexcept;
	static void ClearSceneLayerAllocationFailureForTesting() noexcept;
	static void FailNextSceneLayerTopologyBatchCommitForTesting() noexcept;
	static void ClearSceneLayerTopologyBatchCommitFailureForTesting() noexcept;
	/** Fails after old group parenting is detached but before new groups exist. */
	static void FailNextSceneLayerGroupTopologyStageForTesting() noexcept;
	static void ClearSceneLayerGroupTopologyStageFailureForTesting() noexcept;
	static bool ExchangeSceneCompositionSurfaceBackendForTesting(
		bool enabled) noexcept;

	IDCompositionDevice* CompositionDevice() const noexcept;
	IDCompositionVisual* WebContainerVisual() const noexcept;
	bool RegisterCompositionVisual(IDCompositionVisual* visual, int layer, int order);
	void UpdateCompositionVisualOrder(IDCompositionVisual* visual, int layer, int order);
	void UnregisterCompositionVisual(IDCompositionVisual* visual);
	bool CommitComposition() noexcept;

private:
	struct SceneLayer
	{
		IDCompositionVisual* Visual = nullptr;
		IDCompositionVisual* ContentVisual = nullptr;
		IDCompositionEffectGroup* EffectGroup = nullptr;
		IDCompositionRectangleClip* Clip = nullptr;
		std::vector<IDCompositionVisual*> IntermediateClipVisuals;
		std::vector<IDCompositionRectangleClip*> IntermediateClips;
		std::unique_ptr<D2DGraphics> Graphics;
		D2DGraphics* Recorder = nullptr;
		SceneLayerVisualProperties VisualProperties;
		SceneLayerSurfaceProperties SurfaceProperties;
		int Layer = 0;
		int Order = 0;
		size_t GroupIndex = static_cast<size_t>(-1);
		bool CreatedInTopologyBatch = false;
		bool UsesCompositionSurface = false;
	};

	struct SceneLayerGroup
	{
		IDCompositionVisual* Visual = nullptr;
		IDCompositionVisual* ContentVisual = nullptr;
		IDCompositionEffectGroup* EffectGroup = nullptr;
		IDCompositionRectangleClip* Clip = nullptr;
		std::vector<IDCompositionVisual*> IntermediateClipVisuals;
		std::vector<IDCompositionRectangleClip*> IntermediateClips;
		SceneLayerGroupProperties Properties;
	};

	struct OpenContext
	{
		D2DGraphics* Context = nullptr;
		SurfaceRole Role = SurfaceRole::Primary;
		bool CommandRecording = false;
		bool ClipPushed = false;
	};

	HWND _window = nullptr;
	UINT _width = 1;
	UINT _height = 1;
	UINT _dpi = 96;
	std::unique_ptr<D2DGraphics> _primary;
	std::unique_ptr<D2DGraphics> _overlay;
	std::unique_ptr<DCompLayeredHost> _composition;
	std::unique_ptr<D2DGraphics> _sceneCommandRecorder;
	std::vector<SceneLayer> _sceneLayers;
	std::vector<SceneLayerGroup> _sceneLayerGroups;
	std::vector<OpenContext> _openContexts;
	D2DGraphics* _active = nullptr;
	bool _recoveringDevice = false;
	bool _deviceResetRequested = false;
	bool _hasPresentedFrame = false;
	bool _transactionOpen = false;
	uint64_t _activeTransactionSequence = 0;
	uint64_t _nextFrameSequence = 0;
	uint64_t _resourceGeneration = 0;
	uint64_t _deviceResourceGeneration = 0;
	uint64_t _sharedDeviceGeneration = 0;
	uint64_t _committedFrames = 0;
	uint64_t _abortedFrames = 0;
	uint64_t _deviceRecoveries = 0;
	uint64_t _injectedDeviceLosses = 0;
	uint64_t _lastSurfaceFailureSequence = 0;
	SurfaceRole _lastFailedSurfaceRole = SurfaceRole::Primary;
	HRESULT _lastFailedEndDrawHr = S_OK;
	HRESULT _lastFailedPresentHr = S_OK;
	RECT _pendingDamage{};
	bool _hasPendingDamage = false;
	bool _fullDamagePending = false;
	RECT _lastPrimaryDirty{};
	bool _lastPrimaryWasFull = false;
	bool _hasLastPrimaryFrame = false;
	static std::atomic<bool> _failNextPrimaryAttachForTesting;
	static std::atomic<uint64_t>
		_failSceneLayerAllocationAfterForTesting;
	static std::atomic<bool>
		_failNextSceneLayerGroupTopologyStageForTesting;
	static std::atomic<bool> _useSceneCompositionSurfaces;
	size_t _peakSceneLayerCount = 0;
	size_t _peakSceneLayerSlotCount = 0;
	uint64_t _peakEstimatedSceneSwapChainBytes = 0;
	uint64_t _peakEstimatedSceneCompositionSurfaceBytes = 0;
	uint64_t _peakEstimatedSceneSubmittedSnapshotBytes = 0;
	uint64_t _peakEstimatedSceneLayerSlotBytes = 0;
	uint64_t _sceneLayerCreateCount = 0;
	uint64_t _sceneLayerResizeCount = 0;
	uint64_t _sceneLayerReleaseCount = 0;
	uint64_t _sceneLayerAllocationFailureCount = 0;
	size_t _sceneLayerPixelReadbackLeaseCount = 0;
	uint64_t _sceneLayerSubmittedSnapshotCreateCount = 0;
	uint64_t _sceneLayerSubmittedSnapshotUpdateCount = 0;
	uint64_t _sceneLayerSubmittedSnapshotCopiedBytes = 0;
	double _sceneLayerSubmittedSnapshotCreateMicroseconds = 0.0;
	double _sceneLayerSubmittedSnapshotCopyMicroseconds = 0.0;
	double _sceneLayerSlotEnsureMicroseconds = 0.0;
	double _sceneLayerTopologyBatchBeginMicroseconds = 0.0;
	double _sceneLayerSwapChainCreateMicroseconds = 0.0;
	double _sceneLayerCompositionSurfaceCreateMicroseconds = 0.0;
	double _sceneLayerVisualCreateMicroseconds = 0.0;
	double _sceneLayerVisualBindMicroseconds = 0.0;
	double _sceneLayerGraphicsCreateMicroseconds = 0.0;
	double _sceneLayerRecorderCreateMicroseconds = 0.0;
	double _sceneLayerDpiSetupMicroseconds = 0.0;
	double _sceneLayerVisualPropertyStageMicroseconds = 0.0;
	double _sceneLayerGroupStageMicroseconds = 0.0;
	double _sceneLayerResourcePeakUpdateMicroseconds = 0.0;
	double _sceneLayerTopologyBatchCommitMicroseconds = 0.0;
	bool _sceneLayerTopologyBatchActive = false;
	size_t _sceneLayerTopologyBatchInitialCount = 0;
	bool _sceneLayerBatchResourceMutationPending = false;
	void ReleaseSceneLayerGroups(
		bool restoreChildren,
		IDCompositionVisual* skipNativeVisual = nullptr) noexcept;

	bool OwnsContext(const D2DGraphics* context) const noexcept;
	RECT ToLogicalRect(const RECT& physical) const noexcept;
	bool OpenSurface(
		FrameTransaction& transaction,
		D2DGraphics* context,
		SurfaceRole role,
		const RECT& logicalDirty,
		const RECT& logicalSurfaceClient,
		bool clearTransparent,
		SurfaceFrame& surface);
	bool ValidateClosedContext(D2DGraphics& context) noexcept;
	void MarkTransactionFailed(FrameTransaction& transaction) noexcept;
	void RemoveOpenContext(D2DGraphics* context, bool commandRecording) noexcept;
	D2DGraphics* FindCommandRecorder(D2DGraphics* presentationContext) noexcept;
	bool CreateCompositionResources();
	void AdvanceResourceGeneration() noexcept;
	void AdvanceDeviceResourceGeneration() noexcept;
	void UpdateSceneResourcePeaks() noexcept;
	void AccumulateSceneLayerSubmittedSnapshotStatistics(
		const SceneLayer& layer) noexcept;
	void ReleaseSceneLayerResources(SceneLayer& layer) noexcept;
	void DiscardSceneLayerTopologyBatch(size_t initialSlotCount) noexcept;
	void AcceptSceneLayerTopologyBatch() noexcept;
};
