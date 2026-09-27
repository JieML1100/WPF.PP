#ifndef CUI_WINDOW_H_INCLUDED
#define CUI_WINDOW_H_INCLUDED
#pragma once
#include <vector>
#include <string>
#include <limits>
#include <optional>
#include <unordered_set>
#include "ContentControl.h"
#include "Application.h"
#include "InputManager.h"
#include "FocusManager.h"
#include "TextCompositionManager.h"

#if defined(_MSC_VER)
#pragma comment(lib, "Dwmapi.lib")
#endif

struct IDCompositionDevice;
struct IDCompositionVisual;
class WindowAccessibleObject;
class WindowUiaProvider;
class PlatformWindowHost;
class PresentationRenderHost;
class PresentationScene;
class AnimationFrameScheduler;
class FocusManager;

namespace cui::framework
{
	struct WindowAccess;
}

/** WPF-compatible top-level chrome contract. */
enum class WindowStyle : uint8_t
{
	None,
	SingleBorderWindow,
	ThreeDBorderWindow,
	ToolWindow,
};

/** WPF-compatible native resize/caption capability contract. */
enum class ResizeMode : uint8_t
{
	NoResize,
	CanMinimize,
	CanResize,
	CanResizeWithGrip,
};

typedef Event<void(class Window*)> WindowClosedEvent;
typedef Event<void(class Window*)> WindowContentRenderedEvent;
typedef Event<void(class Window*)> WindowLocationChangedEvent;
typedef Event<void(class Window*, CancelEventArgs&)> WindowCloseEvent;

/**
 * Behavior-only registration for a visual projected above Window content.
 * This is presentation state, never an authored XAML property or ownership
 * relationship. The registered root remains owned by its logical/template tree.
 */
struct TransientPresentationOptions final
{
	bool DismissOnOutsidePointerDown = true;
	bool DismissOnWindowDeactivation = true;
	bool CloseExistingDismissiblePresentation = true;
};

using TransientPresentationDismissHandler = void(*)(class Control&);

/**
 * @file Window.h
 * @brief 顶层 Window 表示层根：协调视觉树、渲染、输入与平台宿主。
 *
 * Window 负责：
 * - 维护窗口语义状态（大小/位置/标题栏按钮等）；HWND 由 PlatformWindowHost 独占
 * - 承载控件树（GetVisualChildrenView()），并进行命中测试/焦点管理
 * - 触发布局（LayoutEngine）以及渲染（D2DGraphics）
 * - 支持 Overlay 渲染与可选的 DirectComposition 容器（供 WebView2 等使用）
 */
class Window : public ContentControl
{
protected:
	std::unique_ptr<AutomationPeer> OnCreateAutomationPeer() override
	{
		return std::make_unique<AutomationPeer>(
			*this, AutomationControlType::Window, L"Window");
	}

private:
	friend class WindowDropTarget;
	friend class Visual;
	friend class Control;
	friend class WindowAccessibleObject;
	friend class WindowUiaProvider;
	friend class FocusManager;
	friend class TextCompositionManager;
	friend class WebBrowser;
	friend class cui::framework::ReverseInheritedProperty;
	friend struct cui::framework::WindowAccess;
	void ApplySystemVisualPreferences(SystemVisualPreferences preferences);
	D2DGraphics* GetCurrentDrawingContext() const noexcept;
	void PublishKeyboardFocusTransition(
		Control* previous,
		Control* current,
		bool invalidateVisual);
	void PublishLogicalFocusTransition(
		Control* previous,
		Control* current);
	void RecordMostRecentInputDevice(const InputReport& input);
	void RefreshKeyboardFocusVisual();
	void RefreshReverseInheritedInputProperties();
	void ClearDetachedControlReferences(Control* root);
	class Button* ResolveDialogButton(bool cancel) const;
	void RefreshDefaultedButtons();
	std::vector<Control*> GetAccessibleControls() const;
	Control* ResolveAccessibleControl(Control* candidate) const noexcept;
	Control* ResolveAccessibleRepresentative(
		Control* control, std::span<Control* const> accessible) const;
	bool TryGetAccessibilityVirtualFocusedNode(
		Control*& owner, uint32_t& virtualId);
	void NotifyAccessibilityEvent(Control* control, AccessibilityChange change);
	void NotifyAccessibilityVirtualEvent(
		Control* owner, uint32_t virtualId, AccessibilityChange change);
	void OnEffectiveIsEnabledChanged(
		bool previousValue, bool currentValue) override;
	void OnEffectiveIsVisibleChanged(
		bool previousValue, bool currentValue) override;
	std::wstring _title = L"Window";
	LRESULT HandleAccessibleObjectRequest(WPARAM wParam, LPARAM lParam);
	float _left = (std::numeric_limits<float>::quiet_NaN)();
	float _top = (std::numeric_limits<float>::quiet_NaN)();
	bool _synchronizingNativeBounds = false;
	EventConnection _windowBoundsChanged;
	::WindowStyle _windowStyle = ::WindowStyle::SingleBorderWindow;
	::ResizeMode _resizeMode = ::ResizeMode::CanResize;
	bool _presentationInvalidated = false;
	enum class CaptionButtonKind : uint8_t { Minimize, Maximize, Close };
	enum class CaptionButtonState : uint8_t { None, Hover, Pressed };
	CaptionButtonState _capMinState = CaptionButtonState::None;
	CaptionButtonState _capMaxState = CaptionButtonState::None;
	CaptionButtonState _capCloseState = CaptionButtonState::None;
	bool _capPressed = false;
	CaptionButtonKind _capPressedKind = CaptionButtonKind::Close;
	bool _capTracking = false;

	D2D1_COLOR_F HeadBackColor = cui::theme::palette::SurfaceMuted;
	D2D1_COLOR_F CaptionHoverColor = cui::theme::palette::AccentSoft;
	D2D1_COLOR_F CaptionPressedColor = cui::theme::palette::AccentSelected;
	D2D1_COLOR_F CloseHoverColor = { 0.90f, 0.20f, 0.20f, 0.50f };
	D2D1_COLOR_F ClosePressedColor = { 0.90f, 0.20f, 0.20f, 0.70f };
	D2D1_COLOR_F BorderLightColor = cui::theme::palette::Surface;
	D2D1_COLOR_F BorderDarkColor = cui::theme::palette::BorderStrong;
	bool _showInTaskbar = true;
	UINT_PTR _animTimerId = 0xC001;
	UINT _animIntervalMs = 0;
	std::unique_ptr<AnimationFrameScheduler> _animationFrameScheduler;
	bool _animationUsesLegacyTimer = false;
	bool(*_animationInputSliceHookForTesting)(void*) noexcept = nullptr;
	void* _animationInputSliceHookContextForTesting = nullptr;
	std::vector<ControlWeakReference> _activeDeclarativeAnimationControls;
	std::vector<ControlWeakReference> _retainedNativeAnimationControls;
	std::vector<ControlWeakReference> _presentationPreparationQueue;
	std::vector<ControlWeakReference> _presentationPreparationScratch;
	/** Drains RequestPresentationPreparation; returns true when work ran. */
	bool RunPresentationPreparation();
	bool _animationRegistryDegraded = false;
	bool _contentRenderedRaised = false;
	CursorKind _currentCursor = CursorKind::Arrow;

	bool TryGetCaptionButtonRect(CaptionButtonKind kind, RECT& out);
	bool HitTestCaptionButtons(POINT ptClient, CaptionButtonKind& outKind);
	bool HitTestCaptionButtonResizeExclusion(POINT ptClient);
	void UpdateCaptionHover(POINT ptClient);
	void ExecuteCaptionButton(CaptionButtonKind kind);
	void ApplyWindowIcon();
	void SynchronizeNativeWindowStyle();
	bool HasWindowChrome() const noexcept;
	bool HasMinimizeBox() const noexcept;
	bool HasMaximizeBox() const noexcept;
	bool CanResizeWindow() const noexcept;
	int GetTitleBarHeightDip() const noexcept;
	void RaiseContentRenderedOnce();
	struct NativeThemeFrame final
	{
		D2D1_COLOR_F WindowBackColor = cui::theme::palette::Window;
		D2D1_COLOR_F WindowForeColor = cui::theme::palette::TextPrimary;
		D2D1_COLOR_F WindowBorderLightColor = cui::theme::palette::Surface;
		D2D1_COLOR_F WindowBorderDarkColor = cui::theme::palette::BorderStrong;
		D2D1_COLOR_F TitleBarBackColor = cui::theme::palette::SurfaceMuted;
		D2D1_COLOR_F CaptionHoverColor = cui::theme::palette::AccentSoft;
		D2D1_COLOR_F CaptionPressedColor = cui::theme::palette::AccentSelected;
		D2D1_COLOR_F CloseHoverColor =
			D2D1_COLOR_F{ 0.90f, 0.20f, 0.20f, 0.50f };
		D2D1_COLOR_F ClosePressedColor =
			D2D1_COLOR_F{ 0.90f, 0.20f, 0.20f, 0.70f };
	};
	struct PresentationBackdropGeometry final
	{
		RECT Damage{};
		RECT ClientFrame{};
	};
	NativeThemeFrame GetNativeThemeFrame() const;
	NativeThemeFrame GetEffectiveNativeThemeFrame() const;
	static PresentationBackdropGeometry ResolvePresentationBackdropGeometry(
		const RECT& logicalDirty,
		const RECT& logicalClient) noexcept;
	void ClearCaptionStates();
	void RefreshAnimationTimer() noexcept;
	bool IsAnimationFrameSchedulerRunningForTesting() const noexcept;
	bool IsAnimationFramePostedForTesting() const noexcept;
	size_t DispatchPendingAnimationInputSlice();
	void InvalidateControl(class Control* control, float inflateDip = 2.0f, bool immediate = false);
	void InvalidateAnimatedControls(bool immediate = false);
	void InvalidateAnimatedControlsAt(
		unsigned long long nowMilliseconds,
		bool immediate = false);
	void SynchronizeActiveDeclarativeAnimationControl(
		class Control* control, bool hasRetainedRoots) noexcept;
	std::vector<class Control*> GetRegisteredDeclarativeAnimationControls();
	std::vector<class Control*>
		GetActiveRegisteredDeclarativeAnimationControls();
	void AdvanceRegisteredDeclarativeAnimationControls(
		std::span<class Control* const> controls,
		unsigned long long nowMilliseconds,
		bool immediate = false);
	void SynchronizeRetainedNativeAnimationControl(
		class Control* control, bool hasRetainedAnimation) noexcept;
	std::vector<class Control*> GetRegisteredNativeAnimationControls();
	std::vector<class Control*> GetActiveRegisteredNativeAnimationControls();
	static bool RectIntersects(const RECT& a, const RECT& b);
	static RECT ToRECT(D2D1_RECT_F rect, int inflatePx = 0);

	void ApplyCursor(CursorKind kind);
	bool ApplySystemCursorId(UINT32 cursorId);
	void UpdateCursor(POINT mouseClient, POINT contentMouse);
	CursorKind QueryCursorAt(POINT mouseClient, POINT contentMouse);
	class Control* HitTestControlAt(POINT contentMouse);
	void UpdateMouseOverProjection(
		class Control* directlyOver, POINT contentMouse,
		bool raiseDirectEvents = true);
	void ReconcileMouseDirectlyOverState();
	void PublishMouseOverTransition(Control& target, bool isMouseOver);
	static HCURSOR GetSystemCursor(CursorKind kind);

	// Window is the root layout boundary for its single semantic content slot.
	bool _resourcesCleaned = false;
	std::unique_ptr<PlatformWindowHost> _platformHost;
	std::unique_ptr<InputManager> _inputManager;
	std::unique_ptr<RoutedCommandManager> _commandManager;
	std::unique_ptr<FocusManager> _focusManager;
	std::unique_ptr<TextCompositionManager> _textCompositionManager;
	std::unique_ptr<PresentationRenderHost> _renderHost;
	std::unique_ptr<PresentationScene> _presentationScene;
	uint64_t _observedResourceGeneration = 0;
	uint64_t _observedDeviceResourceGeneration = 0;
	struct TransientPresentationEntry final
	{
		ControlWeakReference Root;
		TransientPresentationOptions Options;
		TransientPresentationDismissHandler Dismiss = nullptr;
	};
	std::vector<TransientPresentationEntry> _transientPresentations;
	void CompactTransientPresentations();
	std::vector<class Control*> GetTransientPresentationRoots();
	void DismissTransientPresentationsForPointer(class Control* hitControl);
	void DismissTransientPresentationsForWindowDeactivation();
	void DismissTransientPresentationsInSubtree(class Control* root);
	void ClearTransientPresentations() noexcept;
	bool SynchronizePresentationScene();
	bool EnsureDCompInitialized();
	void SynchronizePresentationResourceGeneration();
	// ---- DPI ----
	UINT _dpi = 96;
	bool _initialDpiApplied = false;
	void SyncRenderSizeToClient();
	SIZE GetNativeClientSizePixels() const noexcept;
	cui::core::Size GetSpecifiedWindowSizeDip() const noexcept;
	void ApplySpecifiedSizeToPlatform();
	void ApplyInitialWindowPosition();
	void SynchronizeNativeClientLayoutSlot();
	void SynchronizeNativePosition();
	void RequestArrangeLayout();
	/** Posts one coalesced presentation turn ahead of lower-priority native input. */
	void ScheduleLayoutDispatch();
	int GetTitleBarHeightPixels() const noexcept;
	RECT GetTitleBarClientPixelRect() const noexcept;
	void BeginWindowLayoutDeferral() noexcept;
	void EndWindowLayoutDeferral(bool performLayout);

protected:
	std::wstring GetSemanticText() const override;
	void RequestLayout() override;

private:
	void PerformPendingLayout() override;
	void PerformLayout();
	void ApplyDpiChange(UINT newDpi);
	void EnsureInitialDpiApplied();
	// 鼠标 Hover/Leave 跟踪
	bool _mouseLeaveTracking = false;
	bool _layoutDispatchPosted = false;
	class Control* _mouseDirectlyOver = nullptr;
	ControlWeakReference _mouseDirectlyOverStateOwner;
	POINT _mouseOverContentPoint{};
	class WindowAccessibleObject* _accessibleObject = nullptr;
	class WindowUiaProvider* _uiaProvider = nullptr;
	SystemVisualPreferences _systemVisualPreferences;
	bool _keyboardMostRecentInputDevice = false;
	bool _closingEventActive = false;
	ControlWeakReference _owner;
	std::optional<bool> _dialogResult;
	bool _showingAsDialog = false;
	bool _lastKeyboardMessageHandled = false;
	bool _hasDirectMessageResult = false;
	LRESULT _directMessageResult = 0;
	// OLE Drag&Drop 支持：用于在拖动悬停时返回接受/不接受光标，并支持文本拖放
	struct IDropTarget* _dropTarget = nullptr;
	bool _dropRegistered = false;
	static void EnsureOleInitialized();
	void EnsureDropTargetRegistered();
	void CleanupResources();
	bool OpenTransientPresentation(
		class Control* root,
		TransientPresentationOptions options,
		TransientPresentationDismissHandler dismiss);
	bool CloseTransientPresentation(class Control* root);
	bool IsTransientPresentationOpen(
		const class Control* root) const noexcept;
	class Control* GetTopmostTransientPresentation() const noexcept;
	size_t GetTransientPresentationCount() const noexcept;
	bool RecoverRenderIfNeeded();
	void RefreshSystemVisualPreferences();
	D2D1_COLOR_F GetEffectiveControlBackColor(
		D2D1_COLOR_F configured) const;
	D2D1_COLOR_F GetEffectiveControlForeColor(
		D2D1_COLOR_F configured) const;
	static std::vector<class Control*> BuildTabOrder(
		std::span<class Control* const> roots);
	void UpdateCursorFromCurrentMouse();

public:
	static void RegisterDependencyProperties();
	static const DependencyProperty& TitleProperty();
	static const DependencyProperty& LeftProperty();
	static const DependencyProperty& TopProperty();
	static const DependencyProperty& TopmostProperty();
	static const DependencyProperty& WindowStyleProperty();
	static const DependencyProperty& ResizeModeProperty();
	static const DependencyProperty& ShowInTaskbarProperty();
	UIClass Type() override { return UIClass::UI_Window; }
	WindowCloseEvent OnClosing = WindowCloseEvent();
	/** WPF Window.LocationChanged projection of the native top-level position. */
	WindowLocationChangedEvent OnLocationChanged;
	/** @brief 已关闭事件。 */
	WindowClosedEvent OnWindowClosed = WindowClosedEvent();
	/** WPF Window.ContentRendered; raised after the first committed frame. */
	WindowContentRenderedEvent ContentRendered;

private:
	InputStagingStatistics GetInputStagingStatistics() const noexcept
	{
		return _inputManager ? _inputManager->Statistics()
			: InputStagingStatistics{};
	}

public:
	/** Native platform projection; HWND ownership belongs to PlatformWindowHost. */
	PROPERTY(std::wstring, Title);
	GET(std::wstring, Title);
	SET(std::wstring, Title);
	READONLY_PROPERTY(HWND, Handle);
	GET(HWND, Handle);
	/** WPF-style managed owner relationship; native HWND projection stays internal. */
	PROPERTY(Window*, Owner);
	GET(Window*, Owner);
	SET(Window*, Owner);
	/** Nullable modal result. Assigning a value closes an active dialog. */
	PROPERTY(std::optional<bool>, DialogResult);
	GET(std::optional<bool>, DialogResult);
	SET(std::optional<bool>, DialogResult);
	__declspec(property(
		put = SetWindowStyleValue,
		get = GetWindowStyleValue)) ::WindowStyle WindowStyle;
	::WindowStyle GetWindowStyleValue();
	void SetWindowStyleValue(::WindowStyle value);
	PROPERTY(::ResizeMode, ResizeMode);
	GET(::ResizeMode, ResizeMode);
	SET(::ResizeMode, ResizeMode);
	/** @brief Returns the current DPI-to-96 scale factor (e.g., 2.0 at 192 DPI). */
	float GetDpiScale() const { return _dpi > 0 ? (_dpi / 96.0f) : 1.0f; }
	/**
	 * @brief Converts a rectangle from content-space DIPs to Win32 client pixels.
	 * @param contentRect Rectangle relative to the content origin, in 96-DPI logical units.
	 * @param inflateDip Optional logical padding applied before conversion.
	 */
	RECT ContentDipRectToClientPixels(const D2D1_RECT_F& contentRect, float inflateDip = 0.0f) const;
	/** WPF-style screen position in DIPs. NaN selects automatic centering. */
	PROPERTY(float, Left);
	GET(float, Left);
	SET(float, Left);
	PROPERTY(float, Top);
	GET(float, Top);
	SET(float, Top);

	PROPERTY(bool, ShowInTaskbar);
	GET(bool, ShowInTaskbar);
	SET(bool, ShowInTaskbar);

	PROPERTY(bool, Topmost);
	GET(bool, Topmost);
	SET(bool, Topmost);
	Window();
	~Window();
	const SystemVisualPreferences& GetSystemVisualPreferences() const noexcept
	{
		return _systemVisualPreferences;
	}
	bool AreSystemAnimationsEnabled() const noexcept
	{
		return _systemVisualPreferences.AnimationsEnabled;
	}
	float GetTextScaleFactor() const noexcept
	{
		return _systemVisualPreferences.TextScaleFactor();
	}
	/** Returns the element that currently owns keyboard focus in this Window. */
	class Control* GetKeyboardFocusedElement() const noexcept
	{
		return _focusManager ? _focusManager->KeyboardFocusedElement() : nullptr;
	}
	/** Moves keyboard focus through the unified InputManager focus route. */
	void SetKeyboardFocus(
		class Control* value,
		bool invalidateVisual = true,
		FocusChangeReason reason = FocusChangeReason::Programmatic);
	/** Nearest logical focus scope; Window is the implicit root scope. */
	class Control* GetFocusScope(class Control* element) const noexcept;
	/** Logical focus remains available while native keyboard focus is elsewhere. */
	class Control* GetLogicalFocusedElement(
		class Control* scope = nullptr) const noexcept;
	bool SetLogicalFocus(
		class Control* scope,
		class Control* element,
		bool moveKeyboardFocus = false,
		bool invalidateVisual = true);
private:
	FocusManagerStatistics GetFocusManagerStatistics() const noexcept
	{
		return _focusManager ? _focusManager->Statistics()
			: FocusManagerStatistics{};
	}
	TextCompositionSnapshot GetTextCompositionSnapshot() const
	{
		return _textCompositionManager
			? _textCompositionManager->Snapshot() : TextCompositionSnapshot{};
	}
	TextCompositionStatistics GetTextCompositionStatistics() const noexcept
	{
		return _textCompositionManager
			? _textCompositionManager->Statistics()
			: TextCompositionStatistics{};
	}
public:
	/**
	 * Queues a control whose PreparePresentation may change the visual topology
	 * (virtualized realization after an interactive scroll). The queue runs at the
	 * start of the next paint, before damage is collected and before the retained
	 * scene synchronizes, so the realized children appear in that same frame.
	 */
	void RequestPresentationPreparation(Control& control);
	bool CaptureMouse(class Control* value);
	bool ReleaseMouseCapture(class Control* expectedOwner = nullptr);
	class Control* GetMouseCaptured() const noexcept;
	/** Returns the effective Tab order (TabIndex, then stable tree order). */
	std::vector<class Control*> GetTabOrder() const;
	/** Moves logical focus, wrapping at either end. */
	bool MoveFocus(bool forward = true);
	bool MoveFocus(FocusNavigationDirection direction);
	/** @brief 以非模态方式显示窗口。 */
	void Show();
	/** @brief 以模态方式显示窗口，并返回可空的 WPF 风格 DialogResult。 */
	std::optional<bool> ShowDialog();
	/** @brief 请求关闭窗口。 */
	void Close();
	/** Current content viewport after Window chrome, in DIPs. */
	cui::core::Size GetContentViewportSizeDip() const noexcept;
protected:
	/** Processes normalized Window-level keyboard and activation input. */
	bool ProcessInput(const InputReport& input) override;
	/** Platform input hook for WPF AccessText '_' semantics. */
	bool ProcessAccessKey(wchar_t key);
	/** Window-level semantic input hook invoked before control-tree dispatch. */
	virtual bool OnPreviewInputReport(const InputReport& input);
	/** Optional native-only hook. Returning a result consumes the message. */
	virtual std::optional<LRESULT> OnPlatformMessage(
		UINT message, WPARAM wParam, LPARAM lParam);
	/** Schedules presentation work; immediate flushing is framework/test-only. */
	void Invalidate(bool immediate = false);
	void Invalidate(const RECT& rect, bool immediate = false);
	void Invalidate(D2D1_RECT_F rect, bool immediate = false);
	IDCompositionDevice* GetDCompDevice() const;
private:
	/** Injects a deterministic device reset through the production recovery path. */
	void InjectPresentationDeviceLossForTesting();
	/** Rotates the shared D3D domain, then drives normal presentation recovery. */
	bool InjectSharedGraphicsDeviceRotationForTesting();

	/** True while the retained renderer or the HWND paint queue has work. */
	bool HasPendingRenderWork() const noexcept;
	/** Registered private message ID exposed only through WindowAccess tests. */
	static UINT GetPresentationDispatchMessageForTesting() noexcept;
	/** True when the retained host itself owns unconsumed physical damage. */
	bool HasPendingPresentationDamage() const noexcept;
	/** Returns the last submitted logical dirty rectangle for diagnostics/tests. */
	bool TryGetLastRenderDirtyRect(RECT& logicalDirty, bool& fullFrame) const noexcept;
	uint64_t GetPresentationSceneRevision() const noexcept;
	uint64_t GetPresentationContentRevision() const noexcept;
	uint64_t GetPresentationGeometryRevision() const noexcept;
	uint64_t GetPresentationCompositionRevision() const noexcept;
	uint64_t GetPresentationResourceGeneration() const noexcept;
	uint64_t GetPresentationTransactionSequence() const noexcept;
	uint64_t GetPresentationCommittedFrameCount() const noexcept;
	uint64_t GetPresentationAbortedFrameCount() const noexcept;
	uint64_t GetPresentationDeviceRecoveryCount() const noexcept;
	bool TryGetPresentationSceneLayerPixelDigestForTesting(
		size_t index,
		UINT& width,
		UINT& height,
		uint64_t& digest,
		size_t& nonTransparentPixels) const noexcept;
	bool AcquirePresentationSceneLayerPixelReadbackLeaseForTesting() noexcept;
	void ReleasePresentationSceneLayerPixelReadbackLeaseForTesting() noexcept;
	uint64_t GetPresentationLastSurfaceFailureSequence() const noexcept;
	uint8_t GetPresentationLastFailedSurfaceRole() const noexcept;
	HRESULT GetPresentationLastFailedEndDrawHr() const noexcept;
	HRESULT GetPresentationLastFailedPresentHr() const noexcept;
	size_t GetPresentationNodeCount() const noexcept;
	size_t GetPresentationDrawingLayerCount() const noexcept;
	size_t GetPresentationOpacityGroupCount() const noexcept;
	size_t GetPresentationGroupedNativeVisualCount() const noexcept;
	PresentationFrameStatistics GetPresentationFrameStatistics() const noexcept;
	bool UpdateDirtyRect(const RECT& dirty, bool force = false);
	IDCompositionVisual* GetWebContainerVisual() const;
	bool RegisterDCompVisual(IDCompositionVisual* visual, int layer, int order);
	void UpdateDCompVisualOrder(IDCompositionVisual* visual, int layer, int order);
	void UnregisterDCompVisual(IDCompositionVisual* visual);
	/** Marks the derived retained scene stale after a structural tree change. */
	void InvalidatePresentationStructure() noexcept;
	/** Routes a non-structural Visual revision into its retained scene node. */
	void InvalidatePresentationNode(
		Control* control,
		PresentationInvalidationKind kind) noexcept;
	int GetPresentationOrder(Control* control);
	bool CommitComposition();
	void ProcessPlatformMessage(UINT message, WPARAM wParam, LPARAM lParam);
	LRESULT HandlePlatformWindowMessage(
		HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
};

#endif // CUI_WINDOW_H_INCLUDED
