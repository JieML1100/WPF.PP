#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d2d1.h>
#include <d2d1_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgiformat.h>
#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

#include "Font.h"
#include "Colors.h"
#include "Factory.h"
#include "BitmapSource.h"

EXTERN_C Font* DefaultFontObject;

struct ID3D11Texture2D;

class D2DGraphics {
public:
	enum class SurfaceKind {
		None,
		Offscreen,
		Compatible,
		Hwnd,
		ExternalBitmap,
		DxgiSwapChain,
		CompositionSurface,
		CommandRecorder
	};

	struct InitOptions {
		SurfaceKind kind = SurfaceKind::None;
		UINT width = 0;
		UINT height = 0;
		IWICBitmap* wicBitmap = nullptr;
		bool takeOwnership = false;
		FLOAT dpiX = 96.0f;
		FLOAT dpiY = 96.0f;
		DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
		D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
	};

	D2DGraphics();
	explicit D2DGraphics(IWICBitmap* bitmap, bool takeOwnership = false);
	explicit D2DGraphics(const BitmapSource* bitmap);
	explicit D2DGraphics(IDXGISwapChain* swapChain);
	/** Creates a targetless recorder in the same Direct2D resource domain. */
	explicit D2DGraphics(ID2D1Device* device);
	explicit D2DGraphics(const InitOptions& options);
	virtual ~D2DGraphics();

	void SetDpi(FLOAT dpiX, FLOAT dpiY);
	virtual void BeginRender();
	virtual void EndRender();
	virtual void ReSize(UINT width, UINT height);
	/**
	 * Declares the complete logical-DIP region updated by the current frame.
	 *
	 * Flip-model swap chains must present this as a dirty rectangle. Presenting
	 * a partially redrawn back buffer as a complete frame lets stale pixels from
	 * alternating buffers replace otherwise unchanged retained content.
	 */
	virtual void SetPresentDirtyRect(const RECT& logicalDirty);
	/**
	 * Returns true when this swap chain must receive a complete redraw.
	 *
	 * A newly created/resized flip chain has no coherent presentation history,
	 * and swap effects without Present1 dirty-region support can never accept a
	 * partial retained redraw safely.
	 */
	virtual bool RequiresFullPresentFrame() const noexcept;
	virtual ID3D11Texture2D* GetSubmittedTextureForReadback() const noexcept
	{
		return nullptr;
	}

	// 设备/交换链丢失（远程桌面重连、显卡切换、驱动重启等）时，渲染对象可能需要上层重建。
	bool IsDeviceLost() const { return _deviceLost; }
	HRESULT GetLastEndDrawHr() const { return _lastEndDrawHr; }
	HRESULT GetLastPresentHr() const { return _lastPresentHr; }
	double GetLastEndDrawMicroseconds() const noexcept
	{
		return _lastEndDrawMicroseconds;
	}
	double GetLastPresentMicroseconds() const noexcept
	{
		return _lastPresentMicroseconds;
	}
	virtual double GetLastSurfaceSubmitMicroseconds() const noexcept
	{
		return 0.0;
	}
	UINT GetPresentSyncInterval() const noexcept
	{
		return _presentSyncInterval;
	}
	bool IsCommandRecording() const noexcept { return _commandRecording; }

	HRESULT EnsureDeviceContext();

	void Clear(D2D1_COLOR_F color);

	ID2D1SolidColorBrush* GetColorBrush(D2D1_COLOR_F newcolor);
	ID2D1SolidColorBrush* GetColorBrush(COLORREF newcolor);
	ID2D1SolidColorBrush* GetColorBrush(int r, int g, int b);
	ID2D1SolidColorBrush* GetColorBrush(float r, float g, float b, float a = 1.0f);

	ID2D1SolidColorBrush* GetBackColorBrush(D2D1_COLOR_F newcolor);
	ID2D1SolidColorBrush* GetBackColorBrush(COLORREF newcolor);
	ID2D1SolidColorBrush* GetBackColorBrush(int r, int g, int b);
	ID2D1SolidColorBrush* GetBackColorBrush(float r, float g, float b, float a = 1.0f);

	void DrawLine(D2D1_POINT_2F p1, D2D1_POINT_2F p2, D2D1_COLOR_F color, float linewidth = 1.0f);
	void DrawLine(D2D1_POINT_2F p1, D2D1_POINT_2F p2, ID2D1Brush* brush, float linewidth = 1.0f);

	void DrawLine(float p1_x, float p1_y, float p2_x, float p2_y, D2D1_COLOR_F color, float linewidth = 1.0f);
	void DrawLine(float p1_x, float p1_y, float p2_x, float p2_y, ID2D1Brush* brush, float linewidth = 1.0f);

	void DrawRect(D2D1_RECT_F rect, D2D1_COLOR_F color, float linewidth = 1.0f);
	void DrawRect(float left, float top, float width, float height, D2D1_COLOR_F color, float linewidth = 1.0f);

	void DrawRoundRect(D2D1_RECT_F rect, D2D1_COLOR_F color, float linewidth = 1.0f, float r = 0.5f);
	void DrawRoundRect(float left, float top, float width, float height, D2D1_COLOR_F color, float linewidth = 1.0f, float r = 0.5f);

	void FillRect(D2D1_RECT_F rect, D2D1_COLOR_F color);
	void FillRect(D2D1_RECT_F rect, ID2D1Brush* brush);

	void FillRect(float left, float top, float width, float height, D2D1_COLOR_F color);
	void FillRect(float left, float top, float width, float height, ID2D1Brush* brush);

	void FillRoundRect(D2D1_RECT_F rect, D2D1_COLOR_F color, float r = 0.5f);
	void FillRoundRect(float left, float top, float width, float height, D2D1_COLOR_F color, float r = 0.5f);

	void DrawEllipse(D2D1_POINT_2F cent, float xr, float yr, D2D1_COLOR_F color, float linewidth = 1.0f);
	void DrawEllipse(float x, float y, float xr, float yr, D2D1_COLOR_F color, float linewidth = 1.0f);

	void FillEllipse(D2D1_POINT_2F cent, float xr, float yr, D2D1_COLOR_F color);
	void FillEllipse(float cx, float cy, float xr, float yr, D2D1_COLOR_F color);

	void DrawGeometry(ID2D1Geometry* geo, D2D1_COLOR_F color, float linewidth = 1.0f);
	void DrawGeometry(ID2D1Geometry* geo, ID2D1Brush* brush, float linewidth = 1.0f);

	void FillGeometry(ID2D1Geometry* geo, D2D1_COLOR_F color);
	void FillGeometry(ID2D1Geometry* geo, ID2D1Brush* brush);

	void FillPie(D2D1_POINT_2F center, float width, float height, float startAngle, float sweepAngle, D2D1_COLOR_F color);
	void FillPie(D2D1_POINT_2F center, float width, float height, float startAngle, float sweepAngle, ID2D1Brush* brush);

	void DrawBitmap(ID2D1Bitmap* bmp, float x = 0.f, float y = 0.f, float opacity = 1.0f);
	void DrawBitmap(ID2D1Bitmap* bmp, D2D1_RECT_F rect, float opacity = 1.0f);
	void DrawBitmap(ID2D1Bitmap* bmp, D2D1_RECT_F destRect, D2D1_RECT_F srcRect, float opacity = 1.0f);

	void DrawBitmap(ID2D1Bitmap* bmp, float x, float y, float w, float h, float opacity = 1.0f);
	void DrawBitmap(ID2D1Bitmap* bmp, float dest_x, float dest_y, float dest_w, float dest_h, float src_x, float src_y, float src_w, float src_h, float opacity = 1.0f);

	void FillOpacityMask(ID2D1Bitmap* mask, D2D1_POINT_2F destPoint, D2D1_COLOR_F color, D2D1_OPACITY_MASK_CONTENT content = D2D1_OPACITY_MASK_CONTENT_GRAPHICS);
	void FillOpacityMask(ID2D1Bitmap* mask, D2D1_RECT_F destRect, D2D1_COLOR_F color, D2D1_OPACITY_MASK_CONTENT content = D2D1_OPACITY_MASK_CONTENT_GRAPHICS);
	void FillOpacityMask(ID2D1Bitmap* mask, D2D1_RECT_F destRect, D2D1_RECT_F srcRect, D2D1_COLOR_F color, D2D1_OPACITY_MASK_CONTENT content = D2D1_OPACITY_MASK_CONTENT_GRAPHICS);

	void FillMesh(ID2D1Mesh* mesh, D2D1_COLOR_F color);

	IDWriteTextLayout* CreateStringLayout(const std::wstring& str, float width, float height, Font* font = nullptr);

	void DrawStringLayout(IDWriteTextLayout* layout, float x, float y, D2D1_COLOR_F color);
	void DrawStringLayout(IDWriteTextLayout* layout, float x, float y, ID2D1Brush* brush);

	void DrawStringLayoutCentered(IDWriteTextLayout* layout, float centerX, float centerY, D2D1_COLOR_F color);
	void DrawStringLayoutCentered(IDWriteTextLayout* layout, float centerX, float centerY, ID2D1Brush* brush);

	void DrawStringLayoutOutlined(IDWriteTextLayout* layout, float x, float y, D2D1_COLOR_F textColor, D2D1_COLOR_F outlineColor);
	void DrawStringLayoutOutlined(IDWriteTextLayout* layout, float x, float y, ID2D1Brush* textBrush, D2D1_COLOR_F outlineColor);

	void DrawStringLayoutCenteredOutlined(IDWriteTextLayout* layout, float centerX, float centerY, D2D1_COLOR_F textColor, D2D1_COLOR_F outlineColor);
	void DrawStringLayoutCenteredOutlined(IDWriteTextLayout* layout, float centerX, float centerY, ID2D1Brush* textBrush, D2D1_COLOR_F outlineColor);

	void DrawStringLayoutEffect(IDWriteTextLayout* layout, float x, float y, D2D1_COLOR_F color, DWRITE_TEXT_RANGE subRange, D2D1_COLOR_F fontBack, Font* font = nullptr);
	void DrawStringLayoutEffect(IDWriteTextLayout* layout, float x, float y, ID2D1Brush* brush, DWRITE_TEXT_RANGE subRange, D2D1_COLOR_F fontBack, Font* font = nullptr);
	void DrawStringLayoutEffect(IDWriteTextLayout* layout, float x, float y, D2D1_COLOR_F color, DWRITE_TEXT_RANGE subRange, ID2D1Brush* effectBrush, Font* font = nullptr);
	void DrawStringLayoutEffect(IDWriteTextLayout* layout, float x, float y, ID2D1Brush* brush, DWRITE_TEXT_RANGE subRange, ID2D1Brush* effectBrush, Font* font = nullptr);

	/** Draws one natural-size, non-wrapping text layout. */
	void DrawString(const std::wstring& str, float x, float y, D2D1_COLOR_F color, Font* font = nullptr);
	/** Draws one natural-size, non-wrapping text layout. */
	void DrawString(const std::wstring& str, float x, float y, ID2D1Brush* brush, Font* font = nullptr);
	/** Draws text inside an explicit layout constraint. */
	void DrawString(const std::wstring& str, float x, float y, float w, float h, D2D1_COLOR_F color, Font* font = nullptr);
	/** Draws text inside an explicit layout constraint. */
	void DrawString(const std::wstring& str, float x, float y, float w, float h, ID2D1Brush* brush, Font* font = nullptr);

	void DrawStringCentered(const std::wstring& str, float centerX, float centerY, D2D1_COLOR_F color, Font* font = nullptr);
	void DrawStringCentered(const std::wstring& str, float centerX, float centerY, ID2D1Brush* brush, Font* font = nullptr);

	void DrawStringOutlined(const std::wstring& str, float x, float y, D2D1_COLOR_F textColor, D2D1_COLOR_F outlineColor, Font* font = nullptr);
	void DrawStringOutlined(const std::wstring& str, float x, float y, ID2D1Brush* textBrush, D2D1_COLOR_F outlineColor, Font* font = nullptr);

	void DrawStringCenteredOutlined(const std::wstring& str, float centerX, float centerY, D2D1_COLOR_F textColor, D2D1_COLOR_F outlineColor, Font* font = nullptr);
	void DrawStringCenteredOutlined(const std::wstring& str, float centerX, float centerY, ID2D1Brush* textBrush, D2D1_COLOR_F outlineColor, Font* font = nullptr);

	void DrawTriangle(D2D1_TRIANGLE triangle, D2D1_COLOR_F color, float width = 1.0f);
	void FillTriangle(D2D1_TRIANGLE triangle, D2D1_COLOR_F color);

	void FillPolygon(std::initializer_list<D2D1_POINT_2F> points, D2D1_COLOR_F color);
	void FillPolygon(std::vector<D2D1_POINT_2F> points, D2D1_COLOR_F color);

	void DrawPolygon(std::initializer_list<D2D1_POINT_2F> points, D2D1_COLOR_F color, float width = 1.0f);
	void DrawPolygon(std::vector<D2D1_POINT_2F> points, D2D1_COLOR_F color, float width = 1.0f);

	void DrawArc(D2D1_POINT_2F center, float size, float sa, float ea, D2D1_COLOR_F color, float width = 1.0f);
	void DrawArcCounter(D2D1_POINT_2F center, float size, float sa, float ea, D2D1_COLOR_F color, float width = 1.0f);

	void PushDrawRect(float left, float top, float width, float height);
	void PopDrawRect();
	/** Composites all enclosed drawing as one WPF-style opacity group. */
	bool PushOpacity(float opacity);
	void PopOpacity();
	/** Pushes an arbitrary filled geometry as an additional local clip. */
	bool PushGeometryClip(ID2D1Geometry* geometry);
	/** Pushes a rectangle transformed into the current control-local space. */
	bool PushTransformedRectangleClip(
		D2D1_RECT_F rect,
		const D2D1_MATRIX_3X2_F& transform);
	/**
	 * Pushes the same rectangle as an axis-aligned clip when both the supplied
	 * transform and the current device transform preserve axes. Returns false
	 * without touching the clip stack otherwise; the caller then falls back to
	 * PushTransformedRectangleClip. A successful push is undone by PopDrawRect.
	 */
	bool TryPushAxisAlignedRectangleClip(
		D2D1_RECT_F rect,
		const D2D1_MATRIX_3X2_F& transform);
	void PopGeometryClip();
	bool PushRoundClip(float left, float top, float width, float height, float radius);
	void PopRoundClip();

	// 本地坐标渲染辅助：设置平移变换 + 裁剪，控件内部以 (0,0) 为原点绘制。
	void PushLocalTransform(float tx, float ty, float clipW, float clipH);
	// 使用完整的局部到目标坐标变换；用于继承容器提供的缩放/平移视图。
	void PushLocalTransform(
		const D2D1_MATRIX_3X2_F& transform,
		float clipW,
		float clipH);
	void PopLocalTransform();

	void SetAntialiasMode(D2D1_ANTIALIAS_MODE antialiasMode);
	void SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE antialiasMode);

	ID2D1RenderTarget* GetRenderTargetRaw() const;
	Microsoft::WRL::ComPtr<ID2D1RenderTarget> GetRenderTarget() const;

	ID2D1DeviceContext* GetDeviceContextRaw() const;
	Microsoft::WRL::ComPtr<ID2D1DeviceContext> GetDeviceContext() const;
	ID2D1Device* GetDeviceRaw() const { return pD2DDevice.Get(); }
	/** Internal presentation diagnostics; the returned pointer is non-owning. */
	IDXGISwapChain* GetSwapChainRaw() const noexcept { return pSwapChain.Get(); }

	/** Records an immutable command list without changing a presentation target. */
	bool BeginCommandRecording();
	HRESULT EndCommandRecording(ID2D1CommandList** commandList);
	void AbortCommandRecording() noexcept;
	void DrawCommandList(ID2D1CommandList* commandList);
	/** Replays a command list displaced by a render-space translation (DIP). */
	void DrawCommandList(ID2D1CommandList* commandList, D2D1_POINT_2F offset);

	ID2D1Bitmap1* CreateBitmapFromDxgiSurface(IDXGISurface* surface);
	void DrawDxgiSurface(IDXGISurface* surface, float x, float y, float width, float height, float opacity = 1.0f);

	IWICBitmap* GetTargetWicBitmap() const;

	ID2D1Bitmap* CreateBitmap(IWICBitmap* wb);
	ID2D1Bitmap* CreateBitmap(IWICFormatConverter* conv, D2D1_BITMAP_PROPERTIES* props = nullptr);
	ID2D1Bitmap* CreateBitmap(const std::shared_ptr<BitmapSource>& bitmapSource);

	ID2D1LinearGradientBrush* CreateLinearGradientBrush(
		D2D1_GRADIENT_STOP* stops,
		unsigned int stopcount,
		D2D1_GAMMA gamma = D2D1_GAMMA_2_2);
	ID2D1RadialGradientBrush* CreateRadialGradientBrush(
		D2D1_GRADIENT_STOP* stops,
		unsigned int stopcount,
		D2D1_POINT_2F center,
		D2D1_GAMMA gamma = D2D1_GAMMA_2_2);
	ID2D1BitmapBrush* CreateBitmapBrush(ID2D1Bitmap* bmp);
	ID2D1SolidColorBrush* CreateSolidColorBrush(D2D1_COLOR_F color);
	D2D1_SIZE_F Size();
	void SetTransform(D2D1_MATRIX_3X2_F matrix);
	void ClearTransform();

	static D2D1_SIZE_F GetTextLayoutSize(IDWriteTextLayout* textLayout);
	static std::shared_ptr<BitmapSource> ToBitmapFromSvg(std::string_view svgText, UINT maxBitmapExtent = 4096);
	static std::shared_ptr<BitmapSource> ToBitmapFromSvg(const char* svgText, UINT maxBitmapExtent = 4096);

protected:
	HRESULT Initialize(const InitOptions& options);
	HRESULT InitializeWithSize(UINT width, UINT height, FLOAT dpiX, FLOAT dpiY, DXGI_FORMAT format, D2D1_ALPHA_MODE alphaMode);
	HRESULT InitializeWithWicBitmap(IWICBitmap* bitmap, bool takeOwnership, FLOAT dpiX, FLOAT dpiY, DXGI_FORMAT format, D2D1_ALPHA_MODE alphaMode);
	HRESULT InitializeWithSwapChain(IDXGISwapChain* swapChain);
	HRESULT InitializeWithSwapChain(
		IDXGISwapChain* swapChain,
		ID2D1Device* sharedDevice);
	HRESULT InitializeCommandRecorder(ID2D1Device* device);

	void ResetTarget();
	HRESULT ConfigDefaultObjects();
	HRESULT EnsureDeviceResources();
	HRESULT CreateTargetBitmapForSize(UINT width, UINT height, FLOAT dpiX, FLOAT dpiY, DXGI_FORMAT format, D2D1_ALPHA_MODE alphaMode);
	HRESULT CreateTargetBitmapForSwapChain(IDXGISwapChain* swapChain);
	HRESULT SyncTargetToWicIfNeeded();

protected:
	Microsoft::WRL::ComPtr<ID2D1Device> pD2DDevice;
	Microsoft::WRL::ComPtr<ID2D1DeviceContext> pDeviceContext;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> pTargetBitmap;
	Microsoft::WRL::ComPtr<IDXGISwapChain> pSwapChain;
	D2D1_MATRIX_3X2_F _targetOriginTransform =
		D2D1::Matrix3x2F::Identity();

	struct SolidBrushKey
	{
		uint32_t R = 0;
		uint32_t G = 0;
		uint32_t B = 0;
		uint32_t A = 0;

		bool operator==(const SolidBrushKey&) const noexcept = default;
	};

	struct SolidBrushKeyHash
	{
		size_t operator()(const SolidBrushKey& key) const noexcept
		{
			size_t value = key.R;
			value = (value * 16777619u) ^ key.G;
			value = (value * 16777619u) ^ key.B;
			return (value * 16777619u) ^ key.A;
		}
	};

	std::unordered_map<SolidBrushKey,
		Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>,
		SolidBrushKeyHash> _solidBrushes;
	Microsoft::WRL::ComPtr<IWICBitmap> pWicTargetBitmap;
	Microsoft::WRL::ComPtr<ID2D1CommandList> _recordingCommandList;

	SurfaceKind surfaceKind = SurfaceKind::None;
	bool wicDirty = false;

	HRESULT _lastEndDrawHr = S_OK;
	HRESULT _lastPresentHr = S_OK;
	double _lastEndDrawMicroseconds = 0.0;
	double _lastPresentMicroseconds = 0.0;
	UINT _presentSyncInterval = 1u;
	bool _deviceLost = false;
	bool _commandRecording = false;
	RECT _presentDirtyRect{};
	bool _hasPresentDirtyRect = false;
	bool _swapChainHasPresentedFrame = false;

	std::vector<D2D1_MATRIX_3X2_F> _transformStack;
	size_t _opacityLayerDepth = 0u;
	std::vector<Microsoft::WRL::ComPtr<ID2D1Geometry>> _geometryClipGeometryStack;

	ID2D1SolidColorBrush* GetImmutableSolidColorBrush(D2D1_COLOR_F color);
	bool SupportsDirtyPresent() const noexcept;
	HRESULT PresentSwapChain(UINT syncInterval, UINT flags);
};

class CompatibleGraphics : public D2DGraphics {
public:
	CompatibleGraphics(D2DGraphics* parent, D2D1_SIZE_F desiredSize);

	void ReSize(UINT width, UINT height) override;
	ID2D1Bitmap* GetBitmap() const;

private:
	HRESULT Initialize(D2DGraphics* parent, D2D1_SIZE_F desiredSize);
	HRESULT RecreateTarget();

private:
	Microsoft::WRL::ComPtr<ID2D1Device> parentDevice;
	D2D1_SIZE_F desiredSize;
};

class HwndGraphics : public D2DGraphics {
private:
	HWND hwnd = nullptr;
	HRESULT InitDevice();
public:
	HwndGraphics(HWND hWnd);
	void ReSize(UINT width, UINT height) override;
	void BeginRender() override;
	void EndRender() override;
};

class CompositionSwapChainGraphics : public D2DGraphics {
public:
	explicit CompositionSwapChainGraphics(
		IDXGISwapChain1* swapChain,
		UINT presentSyncInterval = 1u,
		ID2D1Device* sharedDevice = nullptr);
	void ReSize(UINT width, UINT height) override;
	void BeginRender() override;
	void EndRender() override;

private:
	Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
};

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGIDevice;

struct GraphicsSharedD3DDeviceInfo {
	uint64_t Generation = 0;
	bool SupportsVideo = false;
	bool IsHardware = false;
	bool IsSoftwareAdapter = false;
	uint32_t FeatureLevel = 0;
	uint32_t VendorId = 0;
	uint32_t DeviceId = 0;
	uint32_t SubSysId = 0;
	uint32_t Revision = 0;
	uint64_t AdapterLuid = 0;
	uint64_t DedicatedVideoMemoryBytes = 0;
	uint64_t DedicatedSystemMemoryBytes = 0;
	uint64_t SharedSystemMemoryBytes = 0;
	wchar_t AdapterDescription[128]{};
};

HRESULT Graphics_EnsureSharedD3DDevice();
HRESULT Graphics_AcquireSharedD3DDevice(
	ID3D11Device** d3dDevice,
	ID3D11DeviceContext** d3dContext,
	IDXGIDevice** dxgiDevice,
	ID2D1Device** d2dDevice,
	GraphicsSharedD3DDeviceInfo* info = nullptr);
/** Lock-free generation probe for render-loop device-domain checks. */
uint64_t Graphics_GetSharedD3DDeviceGeneration() noexcept;
/** Deterministically rotates the shared device registry for recovery tests. */
HRESULT Graphics_RotateSharedD3DDeviceForTesting(
	GraphicsSharedD3DDeviceInfo* info = nullptr);
/** Test-only driver selection applied by the next shared-device rotation. */
void Graphics_SetForceWarpSharedD3DDeviceForTesting(bool forceWarp) noexcept;
ID3D11Device* Graphics_GetSharedD3DDevice();
IDXGIDevice* Graphics_GetSharedDXGIDevice();
