#include "Graphics.h"
#include "SvgParserInternal.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <numbers>

#include <d2derr.h>

#include <d3d11.h>
#include <d3d10_1.h>
#include <dxgi1_2.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
constexpr float INV_255_1 = 1.0f / 255.0f;
constexpr float DEG_TO_RAD = std::numbers::pi_v<float> / 180.0f;
constexpr float OUTLINE_OFFSET = 1.0f;

namespace {
	class GraphicsWorkClock final {
	public:
		GraphicsWorkClock() noexcept { (void)::QueryPerformanceCounter(&_start); }
		double ElapsedMicroseconds() const noexcept {
			LARGE_INTEGER now{};
			(void)::QueryPerformanceCounter(&now);
			static const LONGLONG frequency = []() noexcept {
				LARGE_INTEGER value{};
				return ::QueryPerformanceFrequency(&value) && value.QuadPart > 0
					? value.QuadPart : LONGLONG{ 1 };
			}();
			return static_cast<double>(now.QuadPart - _start.QuadPart)
				* 1'000'000.0 / static_cast<double>(frequency);
		}
	private:
		LARGE_INTEGER _start{};
	};

	// DirectWrite stores drawing effects on the reusable text layout. Selection
	// foreground is a one-frame paint concern (as in WPF's text selection
	// renderer), so every effect draw must leave the caller's layout clean.
	class DrawingEffectResetScope final {
	public:
		explicit DrawingEffectResetScope(IDWriteTextLayout* layout) noexcept
			: _layout(layout) {}
		~DrawingEffectResetScope()
		{
			if (_layout)
				(void)_layout->SetDrawingEffect(
					nullptr, DWRITE_TEXT_RANGE{ 0, UINT_MAX });
		}

		DrawingEffectResetScope(const DrawingEffectResetScope&) = delete;
		DrawingEffectResetScope& operator=(
			const DrawingEffectResetScope&) = delete;

	private:
		IDWriteTextLayout* _layout = nullptr;
	};

	struct SharedD2D11Resources {
		std::mutex mutex;
		ComPtr<ID3D11Device> d3dDevice;
		ComPtr<ID3D11DeviceContext> d3dContext;
		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<ID2D1Device> d2dDevice;
		uint64_t generation = 0;
		std::atomic<uint64_t> publishedGeneration{ 0 };
		GraphicsSharedD3DDeviceInfo deviceInfo{};
		bool forceWarpForTesting = false;
	};

	SharedD2D11Resources& Shared() {
		static SharedD2D11Resources s;
		return s;
	}

	bool IsDeviceRemovedHr(HRESULT hr) {
		return hr == DXGI_ERROR_DEVICE_REMOVED
			|| hr == DXGI_ERROR_DEVICE_RESET
			|| hr == DXGI_ERROR_DEVICE_HUNG
			|| hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
	}

	void ResetSharedDeviceLocked(SharedD2D11Resources& s) {
		s.d2dDevice.Reset();
		s.dxgiDevice.Reset();
		s.d3dContext.Reset();
		s.d3dDevice.Reset();
		s.deviceInfo = {};
	}

	HRESULT CreateSharedDeviceIfNeededLocked(SharedD2D11Resources& s) {
		if (s.d3dDevice) {
			HRESULT reason = s.d3dDevice->GetDeviceRemovedReason();
			if (FAILED(reason)) {
				ResetSharedDeviceLocked(s);
			}
		}
		if (s.d2dDevice && s.dxgiDevice && s.d3dContext && s.d3dDevice) {
			return S_OK;
		}

		UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if defined(_DEBUG)
		flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		D3D_FEATURE_LEVEL featureLevels[] = {
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
		};
		D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_10_0;

		ComPtr<ID3D11Device> dev;
		ComPtr<ID3D11DeviceContext> ctx;
		bool supportsVideo = true;
		bool isHardware = !s.forceWarpForTesting;
		auto tryCreate = [&](D3D_DRIVER_TYPE driverType,
			UINT attemptFlags, bool& videoSupport) {
			dev.Reset();
			ctx.Reset();
			videoSupport =
				(attemptFlags & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) != 0;
			HRESULT result = D3D11CreateDevice(
				nullptr, driverType, nullptr, attemptFlags,
				featureLevels, _countof(featureLevels), D3D11_SDK_VERSION,
				&dev, &obtained, &ctx);
			if (FAILED(result) && videoSupport) {
				dev.Reset();
				ctx.Reset();
				videoSupport = false;
				result = D3D11CreateDevice(
					nullptr, driverType, nullptr,
					attemptFlags & ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
					featureLevels, _countof(featureLevels),
					D3D11_SDK_VERSION, &dev, &obtained, &ctx);
			}
			return result;
		};
		auto tryCreateWithDebugFallback = [&](D3D_DRIVER_TYPE driverType,
			bool& videoSupport) {
			HRESULT result = tryCreate(driverType, flags, videoSupport);
#if defined(_DEBUG)
			// Debug builds must remain runnable on machines where the optional
			// Windows Graphics Tools feature (D3D debug layer) is not installed.
			if (FAILED(result) && (flags & D3D11_CREATE_DEVICE_DEBUG))
				result = tryCreate(
					driverType, flags & ~D3D11_CREATE_DEVICE_DEBUG,
					videoSupport);
#endif
			return result;
		};

		HRESULT hr = s.forceWarpForTesting
			? E_FAIL
			: tryCreateWithDebugFallback(
				D3D_DRIVER_TYPE_HARDWARE, supportsVideo);
		if (FAILED(hr)) {
			isHardware = false;
			hr = tryCreateWithDebugFallback(
				D3D_DRIVER_TYPE_WARP, supportsVideo);
		}
		if (FAILED(hr)) {
			return hr;
		}
		ComPtr<ID3D10Multithread> multithread;
		if (SUCCEEDED(ctx.As(&multithread)) && multithread) {
			multithread->SetMultithreadProtected(TRUE);
		}
		else {
			return E_NOINTERFACE;
		}

		ComPtr<ID3D11VideoDevice> videoDevice;
		if (!supportsVideo || FAILED(dev.As(&videoDevice)) || !videoDevice) {
			supportsVideo = false;
		}

		ComPtr<IDXGIDevice> dxgiDevice;
		hr = dev.As(&dxgiDevice);
		if (FAILED(hr)) {
			return hr;
		}

		ComPtr<ID2D1Device> d2dDevice;
		auto* d2dFactory = Factory::D2DFactory();
		if (!d2dFactory) return E_FAIL;
		hr = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
		if (FAILED(hr)) {
			return hr;
		}

		s.d3dDevice = dev;
		s.d3dContext = ctx;
		s.dxgiDevice = dxgiDevice;
		s.d2dDevice = d2dDevice;
		++s.generation;
		if (s.generation == 0) ++s.generation;
		GraphicsSharedD3DDeviceInfo deviceInfo{};
		deviceInfo.Generation = s.generation;
		deviceInfo.SupportsVideo = supportsVideo;
		deviceInfo.IsHardware = isHardware;
		deviceInfo.FeatureLevel = static_cast<uint32_t>(obtained);
		ComPtr<IDXGIAdapter> adapter;
		ComPtr<IDXGIAdapter1> adapter1;
		DXGI_ADAPTER_DESC1 adapterDescription{};
		if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))
			&& adapter && SUCCEEDED(adapter.As(&adapter1)) && adapter1
			&& SUCCEEDED(adapter1->GetDesc1(&adapterDescription)))
		{
			deviceInfo.IsSoftwareAdapter =
				(adapterDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
			deviceInfo.VendorId = adapterDescription.VendorId;
			deviceInfo.DeviceId = adapterDescription.DeviceId;
			deviceInfo.SubSysId = adapterDescription.SubSysId;
			deviceInfo.Revision = adapterDescription.Revision;
			deviceInfo.AdapterLuid =
				(static_cast<uint64_t>(static_cast<uint32_t>(
					adapterDescription.AdapterLuid.HighPart)) << 32)
				| static_cast<uint32_t>(
					adapterDescription.AdapterLuid.LowPart);
			deviceInfo.DedicatedVideoMemoryBytes =
				static_cast<uint64_t>(adapterDescription.DedicatedVideoMemory);
			deviceInfo.DedicatedSystemMemoryBytes =
				static_cast<uint64_t>(adapterDescription.DedicatedSystemMemory);
			deviceInfo.SharedSystemMemoryBytes =
				static_cast<uint64_t>(adapterDescription.SharedSystemMemory);
			(void)::wcsncpy_s(
				deviceInfo.AdapterDescription,
				adapterDescription.Description, _TRUNCATE);
		}
		s.deviceInfo = deviceInfo;
		s.publishedGeneration.store(s.generation, std::memory_order_release);
		return S_OK;
	}

	HRESULT CreateSharedDeviceIfNeeded() {
		auto& s = Shared();
		std::scoped_lock lock(s.mutex);
		return CreateSharedDeviceIfNeededLocked(s);
	}

	bool WritePixelsToWicBitmap(IWICBitmap* dst, const void* src, UINT stride, UINT width, UINT height) {
		if (!dst || !src || width == 0 || height == 0 || stride == 0) {
			return false;
		}

		WICRect rect{ 0,0, static_cast<INT>(width), static_cast<INT>(height) };
		ComPtr<IWICBitmapLock> lock;
		if (FAILED(dst->Lock(&rect, WICBitmapLockWrite, &lock))) {
			return false;
		}
		UINT dstStride = 0;
		UINT dstSize = 0;
		BYTE* dstPtr = nullptr;
		if (FAILED(lock->GetStride(&dstStride)) || FAILED(lock->GetDataPointer(&dstSize, &dstPtr)) || !dstPtr) {
			return false;
		}

		const BYTE* srcBytes = static_cast<const BYTE*>(src);
		UINT copyStride = std::min<UINT>(dstStride, stride);
		for (UINT y = 0; y < height; ++y) {
			memcpy(dstPtr + y * dstStride, srcBytes + y * stride, copyStride);
		}
		return true;
	}
}

HRESULT Graphics_EnsureSharedD3DDevice() {
	return CreateSharedDeviceIfNeeded();
}

HRESULT Graphics_AcquireSharedD3DDevice(
	ID3D11Device** d3dDevice,
	ID3D11DeviceContext** d3dContext,
	IDXGIDevice** dxgiDevice,
	ID2D1Device** d2dDevice,
	GraphicsSharedD3DDeviceInfo* info) {
	if (d3dDevice) *d3dDevice = nullptr;
	if (d3dContext) *d3dContext = nullptr;
	if (dxgiDevice) *dxgiDevice = nullptr;
	if (d2dDevice) *d2dDevice = nullptr;
	if (info) *info = {};

	auto& s = Shared();
	std::scoped_lock lock(s.mutex);
	HRESULT hr = CreateSharedDeviceIfNeededLocked(s);
	if (FAILED(hr)) return hr;

	if (d3dDevice) s.d3dDevice.CopyTo(d3dDevice);
	if (d3dContext) s.d3dContext.CopyTo(d3dContext);
	if (dxgiDevice) s.dxgiDevice.CopyTo(dxgiDevice);
	if (d2dDevice) s.d2dDevice.CopyTo(d2dDevice);
	if (info) *info = s.deviceInfo;
	return S_OK;
}

uint64_t Graphics_GetSharedD3DDeviceGeneration() noexcept {
	return Shared().publishedGeneration.load(std::memory_order_acquire);
}

HRESULT Graphics_RotateSharedD3DDeviceForTesting(
	GraphicsSharedD3DDeviceInfo* info) {
	if (info) *info = {};
	auto& s = Shared();
	std::scoped_lock lock(s.mutex);
	// Rotation is a transaction: a failed replacement must leave every caller
	// on the previous healthy domain instead of clearing the registry and
	// allowing a later acquire to split live hosts across generations.
	auto previousD3DDevice = s.d3dDevice;
	auto previousD3DContext = s.d3dContext;
	auto previousDxgiDevice = s.dxgiDevice;
	auto previousD2DDevice = s.d2dDevice;
	const auto previousDeviceInfo = s.deviceInfo;
	ResetSharedDeviceLocked(s);
	const HRESULT hr = CreateSharedDeviceIfNeededLocked(s);
	if (FAILED(hr)) {
		s.d3dDevice = std::move(previousD3DDevice);
		s.d3dContext = std::move(previousD3DContext);
		s.dxgiDevice = std::move(previousDxgiDevice);
		s.d2dDevice = std::move(previousD2DDevice);
		s.deviceInfo = previousDeviceInfo;
		return hr;
	}
	if (info) *info = s.deviceInfo;
	return S_OK;
}

void Graphics_SetForceWarpSharedD3DDeviceForTesting(bool forceWarp) noexcept {
	auto& s = Shared();
	std::scoped_lock lock(s.mutex);
	s.forceWarpForTesting = forceWarp;
}

ID3D11Device* Graphics_GetSharedD3DDevice() {
	thread_local ComPtr<ID3D11Device> snapshot;
	snapshot.Reset();
	(void)Graphics_AcquireSharedD3DDevice(
		snapshot.ReleaseAndGetAddressOf(), nullptr, nullptr, nullptr, nullptr);
	return snapshot.Get();
}

IDXGIDevice* Graphics_GetSharedDXGIDevice() {
	thread_local ComPtr<IDXGIDevice> snapshot;
	snapshot.Reset();
	(void)Graphics_AcquireSharedD3DDevice(
		nullptr, nullptr, snapshot.ReleaseAndGetAddressOf(), nullptr, nullptr);
	return snapshot.Get();
}

namespace {
	Font* DefaultFontObject1() {
		static Font* defaultFont = new Font(L"Arial", 18.0f);
		return defaultFont;
	}
}

D2DGraphics::D2DGraphics() = default;

D2DGraphics::D2DGraphics(IWICBitmap* bitmap, bool takeOwnership) {
	InitializeWithWicBitmap(
		bitmap,
		takeOwnership,
		96.0f,
		96.0f,
		DXGI_FORMAT_B8G8R8A8_UNORM,
		D2D1_ALPHA_MODE_PREMULTIPLIED);
}

D2DGraphics::D2DGraphics(const BitmapSource* bitmap) {
	InitializeWithWicBitmap(
		bitmap ? bitmap->GetWicBitmap() : nullptr,
		false,
		96.0f,
		96.0f,
		DXGI_FORMAT_B8G8R8A8_UNORM,
		D2D1_ALPHA_MODE_PREMULTIPLIED);
}

D2DGraphics::D2DGraphics(IDXGISwapChain* swapChain) {
	(void)InitializeWithSwapChain(swapChain);
}

D2DGraphics::D2DGraphics(ID2D1Device* device) {
	(void)InitializeCommandRecorder(device);
}

D2DGraphics::D2DGraphics(const InitOptions& options) {
	Initialize(options);
}

D2DGraphics::~D2DGraphics() = default;

void D2DGraphics::SetDpi(FLOAT dpiX, FLOAT dpiY) {
	if (pDeviceContext) {
		pDeviceContext->SetDpi(dpiX, dpiY);
	}
}

HRESULT D2DGraphics::EnsureDeviceContext() {
	return EnsureDeviceResources();
}

HRESULT D2DGraphics::EnsureDeviceResources() {
	if (pD2DDevice && pDeviceContext) {
		return S_OK;
	}

	ComPtr<ID2D1Device> sharedD2DDevice;
	HRESULT hr = Graphics_AcquireSharedD3DDevice(
		nullptr, nullptr, nullptr, sharedD2DDevice.GetAddressOf(), nullptr);
	if (FAILED(hr)) {
		return hr;
	}

	pD2DDevice = sharedD2DDevice;
	if (!pD2DDevice) {
		return E_FAIL;
	}

	ComPtr<ID2D1DeviceContext> dc;
	hr = pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
	if (FAILED(hr)) {
		return hr;
	}
	pDeviceContext = dc;
	return S_OK;
}

void D2DGraphics::ResetTarget() {
	if (_commandRecording) {
		AbortCommandRecording();
	}
	_transformStack.clear();
	_opacityLayerDepth = 0u;
	_geometryClipGeometryStack.clear();
	pSwapChain.Reset();
	pTargetBitmap.Reset();
	pDeviceContext.Reset();
	pD2DDevice.Reset();

	pWicTargetBitmap.Reset();
	_solidBrushes.clear();
	_recordingCommandList.Reset();

	surfaceKind = SurfaceKind::None;
	wicDirty = false;
	_commandRecording = false;
	_presentDirtyRect = {};
	_hasPresentDirtyRect = false;
	_swapChainHasPresentedFrame = false;
	_targetOriginTransform = D2D1::Matrix3x2F::Identity();
}

HRESULT D2DGraphics::ConfigDefaultObjects() {
	return pDeviceContext ? S_OK : E_FAIL;
}

HRESULT D2DGraphics::Initialize(const InitOptions& options) {
	HRESULT hr = S_OK;
	switch (options.kind) {
	case SurfaceKind::Offscreen:
		hr = InitializeWithSize(options.width, options.height, options.dpiX, options.dpiY, options.format, options.alphaMode);
		break;
	case SurfaceKind::ExternalBitmap:
		hr = InitializeWithWicBitmap(options.wicBitmap, options.takeOwnership, options.dpiX, options.dpiY, options.format, options.alphaMode);
		break;
	case SurfaceKind::None:
	case SurfaceKind::Compatible:
	case SurfaceKind::Hwnd:
	case SurfaceKind::DxgiSwapChain:
	case SurfaceKind::CompositionSurface:
	case SurfaceKind::CommandRecorder:
	default:
		hr = E_INVALIDARG;
		break;
	}
	return hr;
}

HRESULT D2DGraphics::CreateTargetBitmapForSize(UINT width, UINT height, FLOAT dpiX, FLOAT dpiY, DXGI_FORMAT format, D2D1_ALPHA_MODE alphaMode) {
	if (!pDeviceContext) {
		return E_FAIL;
	}
	if (width == 0 || height == 0) {
		return E_INVALIDARG;
	}

	D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_TARGET,
		D2D1::PixelFormat(format, alphaMode),
		dpiX,
		dpiY);

	ComPtr<ID2D1Bitmap1> bmp;
	HRESULT hr = pDeviceContext->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &props, &bmp);
	if (FAILED(hr)) {
		return hr;
	}

	pTargetBitmap = bmp;
	pDeviceContext->SetTarget(pTargetBitmap.Get());
	return S_OK;
}

HRESULT D2DGraphics::InitializeWithSize(UINT width, UINT height, FLOAT dpiX, FLOAT dpiY, DXGI_FORMAT format, D2D1_ALPHA_MODE alphaMode) {
	if (width == 0 || height == 0) {
		return E_INVALIDARG;
	}
	ResetTarget();

	HRESULT hr = EnsureDeviceResources();
	if (FAILED(hr)) {
		return hr;
	}

	hr = CreateTargetBitmapForSize(width, height, dpiX, dpiY, format, alphaMode);
	if (FAILED(hr)) {
		return hr;
	}

	ComPtr<IWICBitmap> wic;
	hr = _ImageFactory->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wic);
	if (FAILED(hr)) {
		return hr;
	}
	pWicTargetBitmap = wic;

	surfaceKind = SurfaceKind::Offscreen;
	wicDirty = true;
	return ConfigDefaultObjects();
}

HRESULT D2DGraphics::InitializeWithWicBitmap(IWICBitmap* bitmap, bool takeOwnership, FLOAT dpiX, FLOAT dpiY, DXGI_FORMAT format, D2D1_ALPHA_MODE alphaMode) {
	if (!bitmap) {
		return E_INVALIDARG;
	}
	ResetTarget();

	if (takeOwnership) {
		pWicTargetBitmap.Attach(bitmap);
	}
	else {
		pWicTargetBitmap = bitmap;
	}

	UINT width = 0, height = 0;
	if (FAILED(pWicTargetBitmap->GetSize(&width, &height)) || width == 0 || height == 0) {
		ResetTarget();
		return E_INVALIDARG;
	}

	HRESULT hr = EnsureDeviceResources();
	if (FAILED(hr)) {
		ResetTarget();
		return hr;
	}

	hr = CreateTargetBitmapForSize(width, height, dpiX, dpiY, format, alphaMode);
	if (FAILED(hr)) {
		ResetTarget();
		return hr;
	}

	WICRect rect{ 0,0, static_cast<INT>(width), static_cast<INT>(height) };
	ComPtr<IWICBitmapLock> lock;
	if (SUCCEEDED(pWicTargetBitmap->Lock(&rect, WICBitmapLockRead, &lock))) {
		UINT stride = 0;
		UINT bufSize = 0;
		BYTE* buf = nullptr;
		if (SUCCEEDED(lock->GetStride(&stride)) && SUCCEEDED(lock->GetDataPointer(&bufSize, &buf)) && buf && stride) {
			pTargetBitmap->CopyFromMemory(nullptr, buf, stride);
		}
	}

	surfaceKind = SurfaceKind::ExternalBitmap;
	wicDirty = false;
	return ConfigDefaultObjects();
}

HRESULT D2DGraphics::CreateTargetBitmapForSwapChain(IDXGISwapChain* swapChain) {
	if (!swapChain || !pDeviceContext) {
		return E_INVALIDARG;
	}
	ComPtr<IDXGISurface> surface;
	HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));
	if (FAILED(hr)) {
		return hr;
	}
	if (!surface) {
		return E_FAIL;
	}

	D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
		D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
		0.0f,
		0.0f);

	ComPtr<ID2D1Bitmap1> bmp;
	hr = pDeviceContext->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bmp);
	if (FAILED(hr)) {
		return hr;
	}
	pTargetBitmap = bmp;
	pDeviceContext->SetTarget(pTargetBitmap.Get());
	return S_OK;
}

HRESULT D2DGraphics::InitializeWithSwapChain(IDXGISwapChain* swapChain) {
	return InitializeWithSwapChain(swapChain, nullptr);
}

HRESULT D2DGraphics::InitializeWithSwapChain(
	IDXGISwapChain* swapChain,
	ID2D1Device* sharedDevice) {
	if (!swapChain) {
		return E_INVALIDARG;
	}
	ResetTarget();
	ComPtr<ID2D1Device> d2dDevice;
	HRESULT hr = S_OK;
	if (sharedDevice) {
		d2dDevice = sharedDevice;
	}
	else {
		ComPtr<IDXGIDevice> dxgiDevice;
		hr = swapChain->GetDevice(IID_PPV_ARGS(&dxgiDevice));
		if (FAILED(hr)) {
			return hr;
		}
		hr = _D2DFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
		if (FAILED(hr)) {
			return hr;
		}
	}

	ComPtr<ID2D1DeviceContext> dc;
	hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
	if (FAILED(hr)) {
		return hr;
	}

	pD2DDevice = d2dDevice;
	pDeviceContext = dc;

	pSwapChain = swapChain;
	hr = CreateTargetBitmapForSwapChain(pSwapChain.Get());
	if (FAILED(hr)) {
		ResetTarget();
		return hr;
	}

	surfaceKind = SurfaceKind::DxgiSwapChain;
	wicDirty = false;
	hr = ConfigDefaultObjects();
	return hr;
}

HRESULT D2DGraphics::InitializeCommandRecorder(ID2D1Device* device) {
	if (!device) return E_INVALIDARG;
	ResetTarget();
	pD2DDevice = device;
	HRESULT hr = pD2DDevice->CreateDeviceContext(
		D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &pDeviceContext);
	if (FAILED(hr)) {
		ResetTarget();
		return hr;
	}
	surfaceKind = SurfaceKind::CommandRecorder;
	return ConfigDefaultObjects();
}

void D2DGraphics::BeginRender() {
	_lastEndDrawHr = S_OK;
	_lastPresentHr = S_OK;
	_lastEndDrawMicroseconds = 0.0;
	_lastPresentMicroseconds = 0.0;
	if (!pDeviceContext) {
		_lastEndDrawHr = E_POINTER;
		return;
	}
	pDeviceContext->BeginDraw();
}

HRESULT D2DGraphics::SyncTargetToWicIfNeeded() {
	if (!pDeviceContext || !pTargetBitmap || !pWicTargetBitmap) {
		return S_OK;
	}
	if (!wicDirty) {
		return S_OK;
	}

	D2D1_SIZE_U size = pTargetBitmap->GetPixelSize();
	if (size.width == 0 || size.height == 0) {
		wicDirty = false;
		return S_OK;
	}

	D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
		pTargetBitmap->GetPixelFormat(),
		96.0f,
		96.0f);

	ComPtr<ID2D1Bitmap1> cpuBitmap;
	HRESULT hr = pDeviceContext->CreateBitmap(size, nullptr, 0, &cpuProps, &cpuBitmap);
	if (FAILED(hr)) {
		return hr;
	}

	hr = cpuBitmap->CopyFromBitmap(nullptr, pTargetBitmap.Get(), nullptr);
	if (FAILED(hr)) {
		return hr;
	}

	D2D1_MAPPED_RECT mapped{};
	hr = cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
	if (FAILED(hr)) {
		return hr;
	}

	bool ok = WritePixelsToWicBitmap(pWicTargetBitmap.Get(), mapped.bits, mapped.pitch, size.width, size.height);
	cpuBitmap->Unmap();

	if (!ok) {
		return E_FAIL;
	}

	wicDirty = false;
	return S_OK;
}

void D2DGraphics::EndRender() {
	if (!pDeviceContext) {
		_lastEndDrawHr = E_POINTER;
		return;
	}
	_lastEndDrawHr = pDeviceContext->EndDraw();
	_lastPresentHr = S_OK;

	if (surfaceKind == SurfaceKind::Offscreen || surfaceKind == SurfaceKind::ExternalBitmap) {
		wicDirty = true;
		if (SUCCEEDED(_lastEndDrawHr))
			_lastEndDrawHr = SyncTargetToWicIfNeeded();
	}

	if (surfaceKind == SurfaceKind::DxgiSwapChain && pSwapChain) {
		_lastPresentHr = PresentSwapChain(1, 0);
	}

	if (_lastEndDrawHr == D2DERR_RECREATE_TARGET
		|| IsDeviceRemovedHr(_lastEndDrawHr)
		|| IsDeviceRemovedHr(_lastPresentHr))
		_deviceLost = true;
}

void D2DGraphics::SetPresentDirtyRect(const RECT& logicalDirty) {
	_hasPresentDirtyRect = false;
	_presentDirtyRect = {};
	if (!pSwapChain || logicalDirty.right <= logicalDirty.left
		|| logicalDirty.bottom <= logicalDirty.top
		|| RequiresFullPresentFrame()) {
		return;
	}

	FLOAT dpiX = 96.0f;
	FLOAT dpiY = 96.0f;
	if (pDeviceContext)
		pDeviceContext->GetDpi(&dpiX, &dpiY);
	const float scaleX = dpiX > 0.0f ? dpiX / 96.0f : 1.0f;
	const float scaleY = dpiY > 0.0f ? dpiY / 96.0f : 1.0f;

	DXGI_SWAP_CHAIN_DESC description{};
	if (FAILED(pSwapChain->GetDesc(&description))) return;
	const LONG width = static_cast<LONG>(description.BufferDesc.Width);
	const LONG height = static_cast<LONG>(description.BufferDesc.Height);
	RECT physical{
		static_cast<LONG>(std::floor(logicalDirty.left * scaleX)),
		static_cast<LONG>(std::floor(logicalDirty.top * scaleY)),
		static_cast<LONG>(std::ceil(logicalDirty.right * scaleX)),
		static_cast<LONG>(std::ceil(logicalDirty.bottom * scaleY)) };
	RECT bounds{ 0, 0, width, height };
	RECT clipped{};
	if (!::IntersectRect(&clipped, &physical, &bounds)) return;
	if (clipped.left == bounds.left && clipped.top == bounds.top
		&& clipped.right == bounds.right
		&& clipped.bottom == bounds.bottom) return;
	_presentDirtyRect = clipped;
	_hasPresentDirtyRect = true;
}

bool D2DGraphics::SupportsDirtyPresent() const noexcept {
	if (!pSwapChain) return false;
	DXGI_SWAP_CHAIN_DESC description{};
	if (FAILED(pSwapChain->GetDesc(&description))) return false;
	if (description.SwapEffect != DXGI_SWAP_EFFECT_SEQUENTIAL
		&& description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
		return false;
	ComPtr<IDXGISwapChain1> swapChain1;
	return SUCCEEDED(pSwapChain.As(&swapChain1)) && swapChain1;
}

bool D2DGraphics::RequiresFullPresentFrame() const noexcept {
	return pSwapChain
		&& (!_swapChainHasPresentedFrame || !SupportsDirtyPresent());
}

HRESULT D2DGraphics::PresentSwapChain(UINT syncInterval, UINT flags) {
	if (!pSwapChain) return S_OK;
	HRESULT result = E_FAIL;
	ComPtr<IDXGISwapChain1> swapChain1;
	if (_hasPresentDirtyRect
		&& SUCCEEDED(pSwapChain.As(&swapChain1)) && swapChain1) {
		DXGI_PRESENT_PARAMETERS parameters{};
		parameters.DirtyRectsCount = 1;
		parameters.pDirtyRects = &_presentDirtyRect;
		result = swapChain1->Present1(syncInterval, flags, &parameters);
	}
	else {
		result = pSwapChain->Present(syncInterval, flags);
	}
	if (result == S_OK) _swapChainHasPresentedFrame = true;
	_presentDirtyRect = {};
	_hasPresentDirtyRect = false;
	return result;
}

void D2DGraphics::ReSize(UINT width, UINT height) {
	if (!pDeviceContext) {
		return;
	}

	width = std::max<UINT>(1, width);
	height = std::max<UINT>(1, height);

	if (surfaceKind == SurfaceKind::Offscreen) {
		InitializeWithSize(width, height, 96.0f, 96.0f, DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
		return;
	}

	if (surfaceKind == SurfaceKind::DxgiSwapChain && pSwapChain) {
		pTargetBitmap.Reset();
		pDeviceContext->SetTarget(nullptr);
		pDeviceContext->Flush();

		_lastPresentHr = pSwapChain->ResizeBuffers(
			0, width, height, DXGI_FORMAT_UNKNOWN, 0);
		if (FAILED(_lastPresentHr)) {
			_deviceLost = true;
			return;
		}
		_lastEndDrawHr = CreateTargetBitmapForSwapChain(pSwapChain.Get());
		if (FAILED(_lastEndDrawHr)) {
			_deviceLost = true;
			return;
		}
		_swapChainHasPresentedFrame = false;
		(void)ConfigDefaultObjects();
		return;
	}
}

void D2DGraphics::Clear(D2D1_COLOR_F color) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	ctx->Clear(color);
	wicDirty = true;
}

ID2D1SolidColorBrush* D2DGraphics::GetImmutableSolidColorBrush(
	D2D1_COLOR_F color) {
	if (!pDeviceContext) return nullptr;
	const SolidBrushKey key{
		std::bit_cast<uint32_t>(color.r),
		std::bit_cast<uint32_t>(color.g),
		std::bit_cast<uint32_t>(color.b),
		std::bit_cast<uint32_t>(color.a) };
	if (const auto found = _solidBrushes.find(key);
		found != _solidBrushes.end()) return found->second.Get();
	ComPtr<ID2D1SolidColorBrush> brush;
	if (FAILED(pDeviceContext->CreateSolidColorBrush(color, &brush)))
		return nullptr;
	auto* result = brush.Get();
	_solidBrushes.emplace(key, std::move(brush));
	return result;
}

ID2D1SolidColorBrush* D2DGraphics::GetColorBrush(D2D1_COLOR_F newcolor) {
	return GetImmutableSolidColorBrush(newcolor);
}
ID2D1SolidColorBrush* D2DGraphics::GetColorBrush(COLORREF newcolor) {
	return GetColorBrush(D2D1_COLOR_F{ GetRValue(newcolor) * INV_255_1,GetGValue(newcolor) * INV_255_1,GetBValue(newcolor) * INV_255_1,1.0f });
}
ID2D1SolidColorBrush* D2DGraphics::GetColorBrush(int r, int g, int b) {
	return GetColorBrush(D2D1_COLOR_F{ r * INV_255_1,g * INV_255_1,b * INV_255_1,1.0f });
}
ID2D1SolidColorBrush* D2DGraphics::GetColorBrush(float r, float g, float b, float a) {
	return GetColorBrush(D2D1_COLOR_F{ r,g,b,a });
}

ID2D1SolidColorBrush* D2DGraphics::GetBackColorBrush(D2D1_COLOR_F newcolor) {
	return GetImmutableSolidColorBrush(newcolor);
}
ID2D1SolidColorBrush* D2DGraphics::GetBackColorBrush(COLORREF newcolor) {
	return GetBackColorBrush(D2D1_COLOR_F{ GetRValue(newcolor) * INV_255_1,GetGValue(newcolor) * INV_255_1,GetBValue(newcolor) * INV_255_1,1.0f });
}
ID2D1SolidColorBrush* D2DGraphics::GetBackColorBrush(int r, int g, int b) {
	return GetBackColorBrush(D2D1_COLOR_F{ r * INV_255_1,g * INV_255_1,b * INV_255_1,1.0f });
}
ID2D1SolidColorBrush* D2DGraphics::GetBackColorBrush(float r, float g, float b, float a) {
	return GetBackColorBrush(D2D1_COLOR_F{ r,g,b,a });
}

// ---- 绘制/文本 API：基本沿用 Graphics.cpp（DeviceContext 继承 RenderTarget）----

void D2DGraphics::DrawLine(D2D1_POINT_2F p1, D2D1_POINT_2F p2, D2D1_COLOR_F color, float linewidth) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->DrawLine(p1, p2, brush, linewidth);
}
void D2DGraphics::DrawLine(D2D1_POINT_2F p1, D2D1_POINT_2F p2, ID2D1Brush* brush, float linewidth) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !brush) return;
	ctx->DrawLine(p1, p2, brush, linewidth);
}
void D2DGraphics::DrawLine(float p1_x, float p1_y, float p2_x, float p2_y, D2D1_COLOR_F color, float linewidth) {
	DrawLine(D2D1::Point2F(p1_x, p1_y), D2D1::Point2F(p2_x, p2_y), color, linewidth);
}
void D2DGraphics::DrawLine(float p1_x, float p1_y, float p2_x, float p2_y, ID2D1Brush* brush, float linewidth) {
	DrawLine(D2D1::Point2F(p1_x, p1_y), D2D1::Point2F(p2_x, p2_y), brush, linewidth);
}
void D2DGraphics::DrawRect(D2D1_RECT_F rect, D2D1_COLOR_F color, float linewidth) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->DrawRectangle(rect, brush, linewidth);
}
void D2DGraphics::DrawRect(float left, float top, float width, float height, D2D1_COLOR_F color, float linewidth) {
	DrawRect(D2D1::RectF(left, top, left + width, top + height), color, linewidth);
}
void D2DGraphics::DrawRoundRect(D2D1_RECT_F rect, D2D1_COLOR_F color, float linewidth, float r) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, r, r), brush, linewidth);
}
void D2DGraphics::DrawRoundRect(float left, float top, float width, float height, D2D1_COLOR_F color, float linewidth, float r) {
	DrawRoundRect(D2D1::RectF(left, top, left + width, top + height), color, linewidth, r);
}
void D2DGraphics::FillRect(D2D1_RECT_F rect, D2D1_COLOR_F color) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->FillRectangle(rect, brush);
	wicDirty = true;
}
void D2DGraphics::FillRect(D2D1_RECT_F rect, ID2D1Brush* brush) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !brush) return;
	ctx->FillRectangle(rect, brush);
	wicDirty = true;
}
void D2DGraphics::FillRect(float left, float top, float width, float height, D2D1_COLOR_F color) {
	FillRect(D2D1::RectF(left, top, left + width, top + height), color);
}
void D2DGraphics::FillRect(float left, float top, float width, float height, ID2D1Brush* brush) {
	FillRect(D2D1::RectF(left, top, left + width, top + height), brush);
}
void D2DGraphics::FillRoundRect(D2D1_RECT_F rect, D2D1_COLOR_F color, float r) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, r, r), brush);
	wicDirty = true;
}
void D2DGraphics::FillRoundRect(float left, float top, float width, float height, D2D1_COLOR_F color, float r) {
	FillRoundRect(D2D1::RectF(left, top, left + width, top + height), color, r);
}
void D2DGraphics::DrawEllipse(D2D1_POINT_2F cent, float xr, float yr, D2D1_COLOR_F color, float linewidth) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->DrawEllipse(D2D1::Ellipse(cent, xr, yr), brush, linewidth);
}
void D2DGraphics::DrawEllipse(float x, float y, float xr, float yr, D2D1_COLOR_F color, float linewidth) {
	DrawEllipse(D2D1::Point2F(x, y), xr, yr, color, linewidth);
}
void D2DGraphics::FillEllipse(D2D1_POINT_2F cent, float xr, float yr, D2D1_COLOR_F color) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->FillEllipse(D2D1::Ellipse(cent, xr, yr), brush);
	wicDirty = true;
}
void D2DGraphics::FillEllipse(float cx, float cy, float xr, float yr, D2D1_COLOR_F color) {
	FillEllipse(D2D1::Point2F(cx, cy), xr, yr, color);
}
void D2DGraphics::DrawGeometry(ID2D1Geometry* geo, D2D1_COLOR_F color, float linewidth) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !geo) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->DrawGeometry(geo, brush, linewidth);
}
void D2DGraphics::DrawGeometry(ID2D1Geometry* geo, ID2D1Brush* brush, float linewidth) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !geo || !brush) return;
	ctx->DrawGeometry(geo, brush, linewidth);
}
void D2DGraphics::FillGeometry(ID2D1Geometry* geo, D2D1_COLOR_F color) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !geo) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->FillGeometry(geo, brush);
	wicDirty = true;
}
void D2DGraphics::FillGeometry(ID2D1Geometry* geo, ID2D1Brush* brush) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !geo || !brush) return;
	ctx->FillGeometry(geo, brush);
	wicDirty = true;
}

namespace {
	ComPtr<ID2D1PathGeometry> CreatePieGeometry(D2D1_POINT_2F center, float width, float height,
		float startAngle, float sweepAngle) {
		ComPtr<ID2D1PathGeometry> geo;
		geo.Attach(Factory::CreateGeomtry());
		if (!geo) return nullptr;

		ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(geo->Open(&sink))) return nullptr;

		sink->BeginFigure(center, D2D1_FIGURE_BEGIN_FILLED);
		float startRad = startAngle * DEG_TO_RAD;
		float endRad = (startAngle + sweepAngle) * DEG_TO_RAD;
		D2D1_POINT_2F startPoint{ center.x + (width * 0.5f) * cosf(startRad), center.y - (height * 0.5f) * sinf(startRad) };
		D2D1_POINT_2F endPoint{ center.x + (width * 0.5f) * cosf(endRad), center.y - (height * 0.5f) * sinf(endRad) };
		sink->AddLine(startPoint);
		D2D1_SIZE_F arcSize{ width * 0.5f, height * 0.5f };
		D2D1_ARC_SIZE arcSizeFlag = (std::fabs(sweepAngle) <= 180.0f) ? D2D1_ARC_SIZE_SMALL : D2D1_ARC_SIZE_LARGE;
		D2D1_SWEEP_DIRECTION sweepDir = sweepAngle >= 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
		sink->AddArc(D2D1::ArcSegment(endPoint, arcSize, 0.0f, sweepDir, arcSizeFlag));
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
		sink->Close();
		return geo;
	}
}

void D2DGraphics::FillPie(D2D1_POINT_2F center, float width, float height, float startAngle, float sweepAngle, D2D1_COLOR_F color) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	auto geo = CreatePieGeometry(center, width, height, startAngle, sweepAngle);
	if (!geo) return;
	ctx->FillGeometry(geo.Get(), brush);
	wicDirty = true;
}

void D2DGraphics::FillPie(D2D1_POINT_2F center, float width, float height, float startAngle, float sweepAngle, ID2D1Brush* brush) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !brush) return;
	auto geo = CreatePieGeometry(center, width, height, startAngle, sweepAngle);
	if (!geo) return;
	ctx->FillGeometry(geo.Get(), brush);
	wicDirty = true;
}

void D2DGraphics::DrawBitmap(ID2D1Bitmap* bmp, float x, float y, float opacity) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !bmp) return;
	D2D1_SIZE_F siz = bmp->GetSize();
	ctx->DrawBitmap(bmp, D2D1::RectF(x, y, siz.width + x, siz.height + y), opacity);
}
void D2DGraphics::DrawBitmap(ID2D1Bitmap* bmp, D2D1_RECT_F rect, float opacity) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !bmp) return;
	ctx->DrawBitmap(bmp, rect, opacity);
}
void D2DGraphics::DrawBitmap(ID2D1Bitmap* bmp, D2D1_RECT_F destRect, D2D1_RECT_F srcRect, float opacity) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !bmp) return;
	ctx->DrawBitmap(bmp, destRect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, srcRect);
}
void D2DGraphics::DrawBitmap(ID2D1Bitmap* bmp, float x, float y, float w, float h, float opacity) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !bmp) return;
	ctx->DrawBitmap(bmp, D2D1::RectF(x, y, w + x, h + y), opacity);
}
void D2DGraphics::DrawBitmap(ID2D1Bitmap* bmp, float dest_x, float dest_y, float dest_w, float dest_h, float src_x, float src_y, float src_w, float src_h, float opacity) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !bmp) return;
	ctx->DrawBitmap(bmp, D2D1::RectF(dest_x, dest_y, dest_w + dest_x, dest_h + dest_y), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, D2D1::RectF(src_x, src_y, src_w + src_x, src_h + src_y));
}

void D2DGraphics::FillOpacityMask(ID2D1Bitmap* mask, D2D1_POINT_2F destPoint, D2D1_COLOR_F color, D2D1_OPACITY_MASK_CONTENT content) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !mask) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	auto bitmapSize = mask->GetSize();
	D2D1_RECT_F maskRect = D2D1::RectF(0, 0, bitmapSize.width, bitmapSize.height);
	D2D1_RECT_F destRect = D2D1::RectF(destPoint.x, destPoint.y, destPoint.x + bitmapSize.width, destPoint.y + bitmapSize.height);
	ctx->FillOpacityMask(mask, brush, content, &destRect, &maskRect);
	wicDirty = true;
}
void D2DGraphics::FillOpacityMask(ID2D1Bitmap* mask, D2D1_RECT_F destRect, D2D1_COLOR_F color, D2D1_OPACITY_MASK_CONTENT content) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !mask) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	auto bitmapSize = mask->GetSize();
	D2D1_RECT_F maskRect = D2D1::RectF(0, 0, bitmapSize.width, bitmapSize.height);
	ctx->FillOpacityMask(mask, brush, content, &destRect, &maskRect);
	wicDirty = true;
}
void D2DGraphics::FillOpacityMask(ID2D1Bitmap* mask, D2D1_RECT_F destRect, D2D1_RECT_F srcRect, D2D1_COLOR_F color, D2D1_OPACITY_MASK_CONTENT content) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !mask) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->FillOpacityMask(mask, brush, content, &destRect, &srcRect);
	wicDirty = true;
}

void D2DGraphics::FillMesh(ID2D1Mesh* mesh, D2D1_COLOR_F color) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !mesh) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->FillMesh(mesh, brush);
	wicDirty = true;
}

IDWriteTextLayout* D2DGraphics::CreateStringLayout(const std::wstring& str, float width, float height, Font* font) {
	IDWriteTextLayout* textLayout = nullptr;
	Font* resolvedFont = font ? font : DefaultFontObject1();
	if (!resolvedFont) return nullptr;
	IDWriteTextFormat* fnt = resolvedFont->FontObject;
	_DWriteFactory->CreateTextLayout(str.c_str(), static_cast<UINT32>(str.size()), fnt, width, height, &textLayout);
	return textLayout;
}

void D2DGraphics::DrawStringLayout(IDWriteTextLayout* layout, float x, float y, D2D1_COLOR_F color) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !layout) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
	wicDirty = true;
}
void D2DGraphics::DrawStringLayout(IDWriteTextLayout* layout, float x, float y, ID2D1Brush* brush) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !layout || !brush) return;
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
	wicDirty = true;
}

void D2DGraphics::DrawStringLayoutCentered(IDWriteTextLayout* layout, float centerX, float centerY, D2D1_COLOR_F color) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !layout) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	D2D1_SIZE_F textSize = GetTextLayoutSize(layout);
	float x = centerX - textSize.width * 0.5f;
	float y = centerY - textSize.height * 0.5f;
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
	wicDirty = true;
}
void D2DGraphics::DrawStringLayoutCentered(IDWriteTextLayout* layout, float centerX, float centerY, ID2D1Brush* brush) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !layout || !brush) return;
	D2D1_SIZE_F textSize = GetTextLayoutSize(layout);
	float x = centerX - textSize.width * 0.5f;
	float y = centerY - textSize.height * 0.5f;
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
	wicDirty = true;
}

namespace {
	void DrawTextOutline(ID2D1DeviceContext* ctx, IDWriteTextLayout* layout, float x, float y,
		ID2D1SolidColorBrush* outlineBrush) {
		ctx->DrawTextLayout(D2D1::Point2F(x - OUTLINE_OFFSET, y - OUTLINE_OFFSET), layout, outlineBrush);
		ctx->DrawTextLayout(D2D1::Point2F(x + OUTLINE_OFFSET, y - OUTLINE_OFFSET), layout, outlineBrush);
		ctx->DrawTextLayout(D2D1::Point2F(x - OUTLINE_OFFSET, y + OUTLINE_OFFSET), layout, outlineBrush);
		ctx->DrawTextLayout(D2D1::Point2F(x + OUTLINE_OFFSET, y + OUTLINE_OFFSET), layout, outlineBrush);
	}

	IDWriteTextLayout* CreateNaturalTextLayout(
		const std::wstring& text,
		Font* font)
	{
		if (!font || !font->FontObject) return nullptr;
		ComPtr<IDWriteTextLayout> layout;
		layout.Attach(Factory::CreateStringLayout(
			text, FLT_MAX, FLT_MAX, font->FontObject));
		if (!layout) return nullptr;
		layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		const auto natural = D2DGraphics::GetTextLayoutSize(layout.Get());
		// The convenience overload means "natural text extent", not an
		// effectively infinite paint rectangle. Callers that own a constrained
		// content slot use the explicit width/height overload instead.
		layout->SetMaxWidth((std::max)(0.01f, natural.width));
		layout->SetMaxHeight((std::max)(0.01f, natural.height));
		return layout.Detach();
	}
}

void D2DGraphics::DrawStringLayoutOutlined(IDWriteTextLayout* layout, float x, float y, D2D1_COLOR_F textColor, D2D1_COLOR_F outlineColor) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !layout) return;
	auto textBrush = GetColorBrush(textColor);
	auto outlineBrush = GetBackColorBrush(outlineColor);
	if (!textBrush || !outlineBrush) return;
	DrawTextOutline(ctx, layout, x, y, outlineBrush);
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, textBrush);
	wicDirty = true;
}

void D2DGraphics::DrawStringLayoutOutlined(IDWriteTextLayout* layout, float x, float y, ID2D1Brush* textBrush, D2D1_COLOR_F outlineColor) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !layout || !textBrush) return;
	auto outlineBrush = GetBackColorBrush(outlineColor);
	if (!outlineBrush) return;
	DrawTextOutline(ctx, layout, x, y, outlineBrush);
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, textBrush);
	wicDirty = true;
}

void D2DGraphics::DrawStringLayoutCenteredOutlined(IDWriteTextLayout* layout, float centerX, float centerY, D2D1_COLOR_F textColor, D2D1_COLOR_F outlineColor) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !layout) return;
	auto textBrush = GetColorBrush(textColor);
	auto outlineBrush = GetBackColorBrush(outlineColor);
	if (!textBrush || !outlineBrush) return;
	D2D1_SIZE_F textSize = GetTextLayoutSize(layout);
	float x = centerX - textSize.width * 0.5f;
	float y = centerY - textSize.height * 0.5f;
	DrawTextOutline(ctx, layout, x, y, outlineBrush);
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, textBrush);
	wicDirty = true;
}

void D2DGraphics::DrawStringLayoutCenteredOutlined(IDWriteTextLayout* layout, float centerX, float centerY, ID2D1Brush* textBrush, D2D1_COLOR_F outlineColor) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !layout || !textBrush) return;
	auto outlineBrush = GetBackColorBrush(outlineColor);
	if (!outlineBrush) return;
	D2D1_SIZE_F textSize = GetTextLayoutSize(layout);
	float x = centerX - textSize.width * 0.5f;
	float y = centerY - textSize.height * 0.5f;
	DrawTextOutline(ctx, layout, x, y, outlineBrush);
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, textBrush);
	wicDirty = true;
}

void D2DGraphics::DrawStringLayoutEffect(IDWriteTextLayout* layout, float x, float y, D2D1_COLOR_F color, DWRITE_TEXT_RANGE subRange, D2D1_COLOR_F fontBack, Font* font) {
	if (!layout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto backBrush = GetBackColorBrush(fontBack);
	auto frontBrush = GetColorBrush(color);
	if (!backBrush || !frontBrush) return;
	DrawingEffectResetScope resetEffect(layout);
	if (FAILED(layout->SetDrawingEffect(
		nullptr, DWRITE_TEXT_RANGE{ 0, UINT_MAX }))
		|| FAILED(layout->SetDrawingEffect(backBrush, subRange))) return;
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, frontBrush);
	wicDirty = true;
}
void D2DGraphics::DrawStringLayoutEffect(IDWriteTextLayout* layout, float x, float y, ID2D1Brush* brush, DWRITE_TEXT_RANGE subRange, D2D1_COLOR_F fontBack, Font* font) {
	if (!layout || !brush) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto backBrush = GetBackColorBrush(fontBack);
	if (!backBrush) return;
	DrawingEffectResetScope resetEffect(layout);
	if (FAILED(layout->SetDrawingEffect(
		nullptr, DWRITE_TEXT_RANGE{ 0, UINT_MAX }))
		|| FAILED(layout->SetDrawingEffect(backBrush, subRange))) return;
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
	wicDirty = true;
}
void D2DGraphics::DrawStringLayoutEffect(IDWriteTextLayout* layout, float x, float y, D2D1_COLOR_F color, DWRITE_TEXT_RANGE subRange, ID2D1Brush* effectBrush, Font* font) {
	if (!layout || !effectBrush) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto frontBrush = GetColorBrush(color);
	if (!frontBrush) return;
	DrawingEffectResetScope resetEffect(layout);
	if (FAILED(layout->SetDrawingEffect(
		nullptr, DWRITE_TEXT_RANGE{ 0, UINT_MAX }))
		|| FAILED(layout->SetDrawingEffect(effectBrush, subRange))) return;
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, frontBrush);
	wicDirty = true;
}
void D2DGraphics::DrawStringLayoutEffect(IDWriteTextLayout* layout, float x, float y, ID2D1Brush* brush, DWRITE_TEXT_RANGE subRange, ID2D1Brush* effectBrush, Font* font) {
	if (!layout || !brush || !effectBrush) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	DrawingEffectResetScope resetEffect(layout);
	if (FAILED(layout->SetDrawingEffect(
		nullptr, DWRITE_TEXT_RANGE{ 0, UINT_MAX }))
		|| FAILED(layout->SetDrawingEffect(effectBrush, subRange))) return;
	ctx->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
	wicDirty = true;
}

void D2DGraphics::DrawString(const std::wstring& str, float x, float y, D2D1_COLOR_F color, Font* font) {
	Font* resolvedFont = font ? font : DefaultFontObject1();
	if (!resolvedFont) return;
	ComPtr<IDWriteTextLayout> textLayout;
	textLayout.Attach(CreateNaturalTextLayout(str, resolvedFont));
	if (!textLayout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->DrawTextLayout(D2D1::Point2F(x, y), textLayout.Get(), brush);
	wicDirty = true;
}
void D2DGraphics::DrawString(const std::wstring& str, float x, float y, ID2D1Brush* brush, Font* font) {
	Font* resolvedFont = font ? font : DefaultFontObject1();
	if (!resolvedFont) return;
	ComPtr<IDWriteTextLayout> textLayout;
	textLayout.Attach(CreateNaturalTextLayout(str, resolvedFont));
	if (!textLayout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !brush) return;
	ctx->DrawTextLayout(D2D1::Point2F(x, y), textLayout.Get(), brush);
	wicDirty = true;
}
void D2DGraphics::DrawString(const std::wstring& str, float x, float y, float w, float h, D2D1_COLOR_F color, Font* font) {
	IDWriteTextLayout* textLayout = CreateStringLayout(str, w, h, font ? font : DefaultFontObject1());
	if (!textLayout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) { textLayout->Release(); return; }
	auto brush = GetColorBrush(color);
	if (!brush) { textLayout->Release(); return; }
	ctx->DrawTextLayout({ x,y }, textLayout, brush);
	textLayout->Release();
	wicDirty = true;
}
void D2DGraphics::DrawString(const std::wstring& str, float x, float y, float w, float h, ID2D1Brush* brush, Font* font) {
	IDWriteTextLayout* textLayout = CreateStringLayout(str, w, h, font ? font : DefaultFontObject1());
	if (!textLayout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !brush) { textLayout->Release(); return; }
	ctx->DrawTextLayout({ x,y }, textLayout, brush);
	textLayout->Release();
	wicDirty = true;
}
void D2DGraphics::DrawStringCentered(const std::wstring& str, float centerX, float centerY, D2D1_COLOR_F color, Font* font) {
	Font* resolvedFont = font ? font : DefaultFontObject1();
	if (!resolvedFont) return;
	ComPtr<IDWriteTextLayout> textLayout;
	textLayout.Attach(CreateNaturalTextLayout(str, resolvedFont));
	if (!textLayout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	D2D1_SIZE_F textSize = GetTextLayoutSize(textLayout.Get());
	float x = centerX - textSize.width * 0.5f;
	float y = centerY - textSize.height * 0.5f;
	ctx->DrawTextLayout({ x, y }, textLayout.Get(), brush);
	wicDirty = true;
}
void D2DGraphics::DrawStringCentered(const std::wstring& str, float centerX, float centerY, ID2D1Brush* brush, Font* font) {
	Font* resolvedFont = font ? font : DefaultFontObject1();
	if (!resolvedFont) return;
	ComPtr<IDWriteTextLayout> textLayout;
	textLayout.Attach(CreateNaturalTextLayout(str, resolvedFont));
	if (!textLayout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !brush) return;
	D2D1_SIZE_F textSize = GetTextLayoutSize(textLayout.Get());
	float x = centerX - textSize.width * 0.5f;
	float y = centerY - textSize.height * 0.5f;
	ctx->DrawTextLayout({ x, y }, textLayout.Get(), brush);
	wicDirty = true;
}
void D2DGraphics::DrawStringOutlined(const std::wstring& str, float x, float y, D2D1_COLOR_F textColor, D2D1_COLOR_F outlineColor, Font* font) {
	Font* resolvedFont = font ? font : DefaultFontObject1();
	if (!resolvedFont) return;
	ComPtr<IDWriteTextLayout> textLayout;
	textLayout.Attach(CreateNaturalTextLayout(str, resolvedFont));
	if (!textLayout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto textBrush = GetColorBrush(textColor);
	auto outlineBrush = GetBackColorBrush(outlineColor);
	if (!textBrush || !outlineBrush) return;
	DrawTextOutline(ctx, textLayout.Get(), x, y, outlineBrush);
	ctx->DrawTextLayout({ x, y }, textLayout.Get(), textBrush);
	wicDirty = true;
}
void D2DGraphics::DrawStringOutlined(const std::wstring& str, float x, float y, ID2D1Brush* textBrush, D2D1_COLOR_F outlineColor, Font* font) {
	Font* resolvedFont = font ? font : DefaultFontObject1();
	if (!resolvedFont) return;
	ComPtr<IDWriteTextLayout> textLayout;
	textLayout.Attach(CreateNaturalTextLayout(str, resolvedFont));
	if (!textLayout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !textBrush) return;
	auto outlineBrush = GetBackColorBrush(outlineColor);
	if (!outlineBrush) return;
	DrawTextOutline(ctx, textLayout.Get(), x, y, outlineBrush);
	ctx->DrawTextLayout({ x, y }, textLayout.Get(), textBrush);
	wicDirty = true;
}
void D2DGraphics::DrawStringCenteredOutlined(const std::wstring& str, float centerX, float centerY, D2D1_COLOR_F textColor, D2D1_COLOR_F outlineColor, Font* font) {
	Font* resolvedFont = font ? font : DefaultFontObject1();
	if (!resolvedFont) return;
	ComPtr<IDWriteTextLayout> textLayout;
	textLayout.Attach(CreateNaturalTextLayout(str, resolvedFont));
	if (!textLayout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto textBrush = GetColorBrush(textColor);
	auto outlineBrush = GetBackColorBrush(outlineColor);
	if (!textBrush || !outlineBrush) return;
	D2D1_SIZE_F textSize = GetTextLayoutSize(textLayout.Get());
	float x = centerX - textSize.width * 0.5f;
	float y = centerY - textSize.height * 0.5f;
	DrawTextOutline(ctx, textLayout.Get(), x, y, outlineBrush);
	ctx->DrawTextLayout({ x, y }, textLayout.Get(), textBrush);
	wicDirty = true;
}
void D2DGraphics::DrawStringCenteredOutlined(const std::wstring& str, float centerX, float centerY, ID2D1Brush* textBrush, D2D1_COLOR_F outlineColor, Font* font) {
	Font* resolvedFont = font ? font : DefaultFontObject1();
	if (!resolvedFont) return;
	ComPtr<IDWriteTextLayout> textLayout;
	textLayout.Attach(CreateNaturalTextLayout(str, resolvedFont));
	if (!textLayout) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !textBrush) return;
	auto outlineBrush = GetBackColorBrush(outlineColor);
	if (!outlineBrush) return;
	D2D1_SIZE_F textSize = GetTextLayoutSize(textLayout.Get());
	float x = centerX - textSize.width * 0.5f;
	float y = centerY - textSize.height * 0.5f;
	DrawTextOutline(ctx, textLayout.Get(), x, y, outlineBrush);
	ctx->DrawTextLayout({ x, y }, textLayout.Get(), textBrush);
	wicDirty = true;
}

void D2DGraphics::FillTriangle(D2D1_TRIANGLE triangle, D2D1_COLOR_F color) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) {
		return;
	}
	auto brush = GetColorBrush(color);
	if (!brush) {
		return;
	}
	ComPtr<ID2D1PathGeometry> geo;
	geo.Attach(Factory::CreateGeomtry());
	if (!geo) {
		return;
	}
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(&sink))) {
		return;
	}

	sink->BeginFigure(triangle.point1, D2D1_FIGURE_BEGIN_FILLED);
	sink->AddLine(triangle.point2);
	sink->AddLine(triangle.point3);
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	ctx->FillGeometry(geo.Get(), brush);
	wicDirty = true;
}
void D2DGraphics::DrawTriangle(D2D1_TRIANGLE triangle, D2D1_COLOR_F color, float width) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ctx->DrawLine(triangle.point1, triangle.point2, brush, width);
	ctx->DrawLine(triangle.point2, triangle.point3, brush, width);
	ctx->DrawLine(triangle.point3, triangle.point1, brush, width);
	wicDirty = true;
}

void D2DGraphics::FillPolygon(std::vector<D2D1_POINT_2F> points, D2D1_COLOR_F color) {
	if (points.size() <= 2) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ComPtr<ID2D1PathGeometry> geo;
	geo.Attach(Factory::CreateGeomtry());
	if (!geo) return;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(&sink))) return;
	sink->BeginFigure(points.front(), D2D1_FIGURE_BEGIN_FILLED);
	sink->AddLines(points.data() + 1, static_cast<UINT32>(points.size() - 1));
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	ctx->FillGeometry(geo.Get(), brush);
	wicDirty = true;
}
void D2DGraphics::FillPolygon(std::initializer_list<D2D1_POINT_2F> points, D2D1_COLOR_F color) {
	if (points.size() <= 2) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ComPtr<ID2D1PathGeometry> geo;
	geo.Attach(Factory::CreateGeomtry());
	if (!geo) return;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(&sink))) return;
	sink->BeginFigure(*points.begin(), D2D1_FIGURE_BEGIN_FILLED);
	sink->AddLines(points.begin() + 1, static_cast<UINT32>(points.size() - 1));
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	ctx->FillGeometry(geo.Get(), brush);
	wicDirty = true;
}
void D2DGraphics::DrawPolygon(std::initializer_list<D2D1_POINT_2F> points, D2D1_COLOR_F color, float width) {
	if (points.size() <= 1) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ComPtr<ID2D1PathGeometry> geo;
	geo.Attach(Factory::CreateGeomtry());
	if (!geo) return;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(&sink))) return;
	sink->BeginFigure(*points.begin(), D2D1_FIGURE_BEGIN_HOLLOW);
	sink->AddLines(points.begin() + 1, static_cast<UINT32>(points.size() - 1));
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
	ctx->DrawGeometry(geo.Get(), brush, width);
	wicDirty = true;
}
void D2DGraphics::DrawPolygon(std::vector<D2D1_POINT_2F> points, D2D1_COLOR_F color, float width) {
	if (points.size() <= 1) return;
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	ComPtr<ID2D1PathGeometry> geo;
	geo.Attach(Factory::CreateGeomtry());
	if (!geo) return;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(&sink))) return;
	sink->BeginFigure(points.front(), D2D1_FIGURE_BEGIN_HOLLOW);
	sink->AddLines(points.data() + 1, static_cast<UINT32>(points.size() - 1));
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
	ctx->DrawGeometry(geo.Get(), brush, width);
	wicDirty = true;
}

void D2DGraphics::DrawArc(D2D1_POINT_2F center, float size, float sa, float ea, D2D1_COLOR_F color, float width) {
	const auto angleToPoint = [](D2D1_POINT_2F cent, float angle, float len) {
		return len > 0 ? D2D1::Point2F(
			cent.x + sinf(angle * DEG_TO_RAD) * len,
			cent.y - cosf(angle * DEG_TO_RAD) * len) : cent;
		};
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	if (size <= 0.0f || width <= 0.0f) return;
	float ts = sa;
	float te = ea;
	float sweepDegrees = te - ts;
	if (sweepDegrees <= 0.0f) {
		sweepDegrees = std::fmod(sweepDegrees, 360.0f) + 360.0f;
	}
	if (sweepDegrees <= 0.001f) return;
	if (sweepDegrees >= 359.999f) {
		ctx->DrawEllipse(D2D1::Ellipse(center, size, size), brush, width);
		wicDirty = true;
		return;
	}
	ComPtr<ID2D1PathGeometry> geo;
	geo.Attach(Factory::CreateGeomtry());
	if (!geo) return;
	if (te < ts) te += 360.0f;
	D2D1_ARC_SIZE sweep = (te - ts < 180.0f) ? D2D1_ARC_SIZE_SMALL : D2D1_ARC_SIZE_LARGE;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(&sink))) return;
	auto start = angleToPoint(center, sa, size);
	auto end = angleToPoint(center, ea, size);
	sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
	sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(size, size), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, sweep));
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
	ctx->DrawGeometry(geo.Get(), brush, width);
	wicDirty = true;
}
void D2DGraphics::DrawArcCounter(D2D1_POINT_2F center, float size, float sa, float ea, D2D1_COLOR_F color, float width) {
	const auto angleToPoint = [](D2D1_POINT_2F cent, float angle, float len) {
		return len > 0 ? D2D1::Point2F(
			cent.x + sinf(angle * DEG_TO_RAD) * len,
			cent.y - cosf(angle * DEG_TO_RAD) * len) : cent;
		};
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	auto brush = GetColorBrush(color);
	if (!brush) return;
	if (size <= 0.0f || width <= 0.0f) return;
	float ts = sa;
	float te = ea;
	float sweepDegrees = te - ts;
	if (sweepDegrees <= 0.0f) {
		sweepDegrees = std::fmod(sweepDegrees, 360.0f) + 360.0f;
	}
	if (sweepDegrees <= 0.001f) return;
	if (sweepDegrees >= 359.999f) {
		ctx->DrawEllipse(D2D1::Ellipse(center, size, size), brush, width);
		wicDirty = true;
		return;
	}
	ComPtr<ID2D1PathGeometry> geo;
	geo.Attach(Factory::CreateGeomtry());
	if (!geo) return;
	if (te < ts) te += 360.0f;
	D2D1_ARC_SIZE sweep = (te - ts < 180.0f) ? D2D1_ARC_SIZE_SMALL : D2D1_ARC_SIZE_LARGE;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(&sink))) return;
	auto start = angleToPoint(center, sa, size);
	auto end = angleToPoint(center, ea, size);
	sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
	sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(size, size), 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, sweep));
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
	ctx->DrawGeometry(geo.Get(), brush, width);
	wicDirty = true;
}

void D2DGraphics::PushDrawRect(float left, float top, float width, float height) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	ctx->PushAxisAlignedClip(D2D1::RectF(left, top, left + width, top + height), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}
void D2DGraphics::PopDrawRect() {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	ctx->PopAxisAlignedClip();
}
bool D2DGraphics::PushOpacity(float opacity) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !std::isfinite(opacity)
		|| opacity < 0.0f || opacity > 1.0f) return false;
	// Device contexts can own and recycle the backing layer. Opacity groups do
	// not require a caller-visible ID2D1Layer, so an explicit CreateLayer here
	// only adds one device-resource allocation to every raster group push.
	auto params = D2D1::LayerParameters1(
		D2D1::InfiniteRect(), nullptr,
		D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
		D2D1::IdentityMatrix(), opacity, nullptr,
		D2D1_LAYER_OPTIONS1_NONE);
	ctx->PushLayer(&params, nullptr);
	++_opacityLayerDepth;
	return true;
}
void D2DGraphics::PopOpacity() {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || _opacityLayerDepth == 0u) return;
	ctx->PopLayer();
	--_opacityLayerDepth;
}
bool D2DGraphics::PushGeometryClip(ID2D1Geometry* geometry) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !geometry) return false;

	// ID2D1DeviceContext can manage its layer resource internally. Passing a
	// null layer lets Direct2D reuse that storage across layers/effect graphs and
	// avoids one device-resource allocation for every retained surface replay.
	// Retain the immutable factory geometry before mutating the D2D stack so an
	// allocation failure cannot leave an unmatched PushLayer in the transaction.
	try
	{
		_geometryClipGeometryStack.emplace_back(geometry);
	}
	catch (...)
	{
		return false;
	}
	auto params = D2D1::LayerParameters1(
		D2D1::InfiniteRect(), geometry,
		D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
		D2D1::IdentityMatrix(), 1.0f, nullptr,
		D2D1_LAYER_OPTIONS1_NONE);
	ctx->PushLayer(&params, nullptr);
	return true;
}
bool D2DGraphics::PushTransformedRectangleClip(
	D2D1_RECT_F rect,
	const D2D1_MATRIX_3X2_F& transform) {
	if (!_D2DFactory || !std::isfinite(rect.left) || !std::isfinite(rect.top)
		|| !std::isfinite(rect.right) || !std::isfinite(rect.bottom)
		|| rect.right <= rect.left || rect.bottom <= rect.top)
		return false;

	ComPtr<ID2D1RectangleGeometry> rectangle;
	if (FAILED(_D2DFactory->CreateRectangleGeometry(rect, &rectangle)))
		return false;
	ComPtr<ID2D1TransformedGeometry> transformed;
	if (FAILED(_D2DFactory->CreateTransformedGeometry(
		rectangle.Get(), &transform, &transformed)))
		return false;
	return PushGeometryClip(transformed.Get());
}
bool D2DGraphics::TryPushAxisAlignedRectangleClip(
	D2D1_RECT_F rect,
	const D2D1_MATRIX_3X2_F& transform) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !std::isfinite(rect.left) || !std::isfinite(rect.top)
		|| !std::isfinite(rect.right) || !std::isfinite(rect.bottom)
		|| rect.right <= rect.left || rect.bottom <= rect.top
		|| transform._12 != 0.0f || transform._21 != 0.0f
		|| !std::isfinite(transform._11) || !std::isfinite(transform._22)
		|| !std::isfinite(transform._31) || !std::isfinite(transform._32))
		return false;
	// PushAxisAlignedClip is exact only while the device transform preserves
	// axes; otherwise Direct2D would clip to the transformed bounding box.
	D2D1_MATRIX_3X2_F current{};
	ctx->GetTransform(&current);
	if (current._12 != 0.0f || current._21 != 0.0f) return false;
	const float x0 = rect.left * transform._11 + transform._31;
	const float x1 = rect.right * transform._11 + transform._31;
	const float y0 = rect.top * transform._22 + transform._32;
	const float y1 = rect.bottom * transform._22 + transform._32;
	const D2D1_RECT_F local{
		(std::min)(x0, x1), (std::min)(y0, y1),
		(std::max)(x0, x1), (std::max)(y0, y1) };
	if (!std::isfinite(local.left) || !std::isfinite(local.top)
		|| !std::isfinite(local.right) || !std::isfinite(local.bottom))
		return false;
	// A rectangle mask in a layer and an axis-aligned clip produce the same
	// per-primitive antialiased coverage, but the latter needs neither factory
	// geometry nor an intermediate layer when the command list is replayed.
	ctx->PushAxisAlignedClip(local, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	return true;
}
void D2DGraphics::PopGeometryClip() {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || _geometryClipGeometryStack.empty()) return;
	ctx->PopLayer();
	_geometryClipGeometryStack.pop_back();
}
bool D2DGraphics::PushRoundClip(float left, float top, float width, float height, float radius) {
	if (!pDeviceContext || width <= 0.0f || height <= 0.0f || radius <= 0.0f) return false;
	radius = (std::clamp)(radius, 0.0f, (std::min)(width, height) * 0.5f);
	if (radius <= 0.0f) return false;

	ComPtr<ID2D1RoundedRectangleGeometry> roundedGeometry;
	auto rect = D2D1::RectF(left, top, left + width, top + height);
	if (FAILED(_D2DFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius), &roundedGeometry)))
		return false;

	return PushGeometryClip(roundedGeometry.Get());
}
void D2DGraphics::PopRoundClip() {
	PopGeometryClip();
}
void D2DGraphics::PushLocalTransform(float tx, float ty, float clipW, float clipH) {
	PushLocalTransform(D2D1::Matrix3x2F::Translation(tx, ty), clipW, clipH);
}
void D2DGraphics::PushLocalTransform(
	const D2D1_MATRIX_3X2_F& transform,
	float clipW,
	float clipH) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	D2D1_MATRIX_3X2_F current;
	ctx->GetTransform(&current);
	_transformStack.push_back(current);
	ctx->SetTransform(transform * _targetOriginTransform);
	ctx->PushAxisAlignedClip(D2D1::RectF(0.f, 0.f, clipW, clipH), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}
void D2DGraphics::PopLocalTransform() {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	ctx->PopAxisAlignedClip();
	if (!_transformStack.empty()) {
		ctx->SetTransform(_transformStack.back());
		_transformStack.pop_back();
	}
}
void D2DGraphics::SetAntialiasMode(D2D1_ANTIALIAS_MODE antialiasMode) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	ctx->SetAntialiasMode(antialiasMode);
}
void D2DGraphics::SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE antialiasMode) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return;
	ctx->SetTextAntialiasMode(antialiasMode);
}

ID2D1RenderTarget* D2DGraphics::GetRenderTargetRaw() const {
	return pDeviceContext.Get();
}
ComPtr<ID2D1RenderTarget> D2DGraphics::GetRenderTarget() const {
	ComPtr<ID2D1RenderTarget> t;
	if (pDeviceContext) {
		pDeviceContext.As(&t);
	}
	return t;
}
ID2D1DeviceContext* D2DGraphics::GetDeviceContextRaw() const {
	return pDeviceContext.Get();
}
ComPtr<ID2D1DeviceContext> D2DGraphics::GetDeviceContext() const {
	return pDeviceContext;
}

bool D2DGraphics::BeginCommandRecording() {
	if (!pDeviceContext || surfaceKind != SurfaceKind::CommandRecorder
		|| _commandRecording) return false;
	_transformStack.clear();
	_opacityLayerDepth = 0u;
	_geometryClipGeometryStack.clear();
	_recordingCommandList.Reset();
	HRESULT hr = pDeviceContext->CreateCommandList(&_recordingCommandList);
	if (FAILED(hr) || !_recordingCommandList) {
		_lastEndDrawHr = FAILED(hr) ? hr : E_FAIL;
		return false;
	}
	pDeviceContext->SetTarget(_recordingCommandList.Get());
	pDeviceContext->SetTransform(D2D1::Matrix3x2F::Identity());
	_lastEndDrawHr = S_OK;
	_lastPresentHr = S_OK;
	pDeviceContext->BeginDraw();
	_commandRecording = true;
	return true;
}

HRESULT D2DGraphics::EndCommandRecording(
	ID2D1CommandList** commandList) {
	if (commandList) *commandList = nullptr;
	if (!_commandRecording || !pDeviceContext || !_recordingCommandList)
		return E_UNEXPECTED;
	_lastEndDrawHr = pDeviceContext->EndDraw();
	pDeviceContext->SetTarget(nullptr);
	_commandRecording = false;
	_transformStack.clear();
	_opacityLayerDepth = 0u;
	_geometryClipGeometryStack.clear();
	if (SUCCEEDED(_lastEndDrawHr))
		_lastEndDrawHr = _recordingCommandList->Close();
	if (_lastEndDrawHr == D2DERR_RECREATE_TARGET
		|| IsDeviceRemovedHr(_lastEndDrawHr))
		_deviceLost = true;
	if (SUCCEEDED(_lastEndDrawHr) && commandList)
		*commandList = _recordingCommandList.Detach();
	else
		_recordingCommandList.Reset();
	return _lastEndDrawHr;
}

void D2DGraphics::AbortCommandRecording() noexcept {
	if (!_commandRecording || !pDeviceContext) {
		_recordingCommandList.Reset();
		_commandRecording = false;
		return;
	}
	_lastEndDrawHr = pDeviceContext->EndDraw();
	pDeviceContext->SetTarget(nullptr);
	_recordingCommandList.Reset();
	_transformStack.clear();
	_opacityLayerDepth = 0u;
	_geometryClipGeometryStack.clear();
	_commandRecording = false;
}

void D2DGraphics::DrawCommandList(ID2D1CommandList* commandList) {
	if (!pDeviceContext || !commandList) return;
	pDeviceContext->DrawImage(commandList);
	wicDirty = true;
}

void D2DGraphics::DrawCommandList(
	ID2D1CommandList* commandList, D2D1_POINT_2F offset) {
	if (!pDeviceContext || !commandList) return;
	if (offset.x == 0.0f && offset.y == 0.0f) {
		DrawCommandList(commandList);
		return;
	}
	D2D1_MATRIX_3X2_F current{};
	pDeviceContext->GetTransform(&current);
	// Command lists carry their recorded world transforms. Displacing the
	// replay first keeps every recorded clip and primitive rigidly together.
	pDeviceContext->SetTransform(
		D2D1::Matrix3x2F::Translation(offset.x, offset.y)
		* D2D1::Matrix3x2F(current._11, current._12, current._21,
			current._22, current._31, current._32));
	pDeviceContext->DrawImage(commandList);
	pDeviceContext->SetTransform(current);
	wicDirty = true;
}

IWICBitmap* D2DGraphics::GetTargetWicBitmap() const {
	return pWicTargetBitmap.Get();
}

ID2D1Bitmap* D2DGraphics::CreateBitmap(IWICBitmap* wb) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !wb) return nullptr;
	ComPtr<ID2D1Bitmap> bitmap;
	if (FAILED(ctx->CreateBitmapFromWicBitmap(wb, &bitmap))) {
		return nullptr;
	}
	return bitmap.Detach();
}
ID2D1Bitmap* D2DGraphics::CreateBitmap(IWICFormatConverter* conv, D2D1_BITMAP_PROPERTIES* props) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !conv) return nullptr;
	ComPtr<ID2D1Bitmap> bitmap;
	if (FAILED(ctx->CreateBitmapFromWicBitmap(conv, props, &bitmap))) {
		return nullptr;
	}
	return bitmap.Detach();
}
ID2D1Bitmap* D2DGraphics::CreateBitmap(const std::shared_ptr<BitmapSource>& bitmapSource) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !bitmapSource) return nullptr;
	ComPtr<ID2D1Bitmap> bitmap;
	if (FAILED(ctx->CreateBitmapFromWicBitmap(bitmapSource->GetWicBitmap(), nullptr, &bitmap))) {
		return nullptr;
	}
	return bitmap.Detach();
}

ID2D1Bitmap1* D2DGraphics::CreateBitmapFromDxgiSurface(IDXGISurface* surface) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !surface) return nullptr;

	D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_NONE,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
		96.0f,
		96.0f);

	ComPtr<ID2D1Bitmap1> bitmap;
	HRESULT hr = ctx->CreateBitmapFromDxgiSurface(surface, &props, &bitmap);
	if (FAILED(hr)) {
		return nullptr;
	}
	return bitmap.Detach();
}

void D2DGraphics::DrawDxgiSurface(IDXGISurface* surface, float x, float y, float width, float height, float opacity) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !surface) return;

	ComPtr<ID2D1Bitmap1> bitmap;
	bitmap.Attach(CreateBitmapFromDxgiSurface(surface));
	if (!bitmap) return;

	D2D1_RECT_F destRect = D2D1::RectF(x, y, x + width, y + height);
	ctx->DrawBitmap(bitmap.Get(), destRect, opacity, D2D1_INTERPOLATION_MODE_LINEAR);
	wicDirty = true;
}

ID2D1LinearGradientBrush* D2DGraphics::CreateLinearGradientBrush(
	D2D1_GRADIENT_STOP* stops,
	unsigned int stopcount,
	D2D1_GAMMA gamma) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !stops || stopcount == 0) return nullptr;
	ComPtr<ID2D1GradientStopCollection> collection;
	if (FAILED(ctx->CreateGradientStopCollection(
		stops, stopcount, gamma, D2D1_EXTEND_MODE_CLAMP, &collection))) return nullptr;
	ComPtr<ID2D1LinearGradientBrush> brush;
	if (FAILED(ctx->CreateLinearGradientBrush({}, collection.Get(), &brush))) return nullptr;
	return brush.Detach();
}
ID2D1RadialGradientBrush* D2DGraphics::CreateRadialGradientBrush(
	D2D1_GRADIENT_STOP* stops,
	unsigned int stopcount,
	D2D1_POINT_2F center,
	D2D1_GAMMA gamma) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !stops || stopcount == 0) return nullptr;
	ComPtr<ID2D1GradientStopCollection> collection;
	if (FAILED(ctx->CreateGradientStopCollection(
		stops, stopcount, gamma, D2D1_EXTEND_MODE_CLAMP, &collection))) return nullptr;
	D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props{};
	props.center = center;
	ComPtr<ID2D1RadialGradientBrush> brush;
	if (FAILED(ctx->CreateRadialGradientBrush(props, collection.Get(), &brush))) return nullptr;
	return brush.Detach();
}
ID2D1BitmapBrush* D2DGraphics::CreateBitmapBrush(ID2D1Bitmap* bmp) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx || !bmp) return nullptr;
	ComPtr<ID2D1BitmapBrush> brush;
	if (FAILED(ctx->CreateBitmapBrush(bmp, nullptr, nullptr, &brush))) return nullptr;
	return brush.Detach();
}
ID2D1SolidColorBrush* D2DGraphics::CreateSolidColorBrush(D2D1_COLOR_F color) {
	auto* ctx = pDeviceContext.Get();
	if (!ctx) return nullptr;
	ComPtr<ID2D1SolidColorBrush> result;
	if (FAILED(ctx->CreateSolidColorBrush(color, &result))) return nullptr;
	return result.Detach();
}

D2D1_SIZE_F D2DGraphics::Size() {
	if (!pDeviceContext) return D2D1::SizeF(0.0f, 0.0f);
	if (pTargetBitmap) {
		auto sz = pTargetBitmap->GetSize();
		return sz;
	}
	return pDeviceContext->GetSize();
}

void D2DGraphics::SetTransform(D2D1_MATRIX_3X2_F matrix) {
	if (pDeviceContext)
		pDeviceContext->SetTransform(matrix * _targetOriginTransform);
}
void D2DGraphics::ClearTransform() {
	if (pDeviceContext) pDeviceContext->SetTransform(_targetOriginTransform);
}

D2D1_SIZE_F D2DGraphics::GetTextLayoutSize(IDWriteTextLayout* textLayout) {
	D2D1_SIZE_F minSize = { 0,0 };
	if (!textLayout) return minSize;
	DWRITE_TEXT_METRICS metrics;
	HRESULT hr = textLayout->GetMetrics(&metrics);
	if (SUCCEEDED(hr)) {
		minSize = D2D1::Size((float)ceil(metrics.widthIncludingTrailingWhitespace), (float)ceil(metrics.height));
		return minSize;
	}
	return minSize;
}

namespace {
	float SvgClamp01(float value) {
		if (!std::isfinite(value)) {
			return 0.0f;
		}
		return std::clamp(value, 0.0f, 1.0f);
	}

	D2D1_COLOR_F SvgColorToD2D(unsigned int color, float opacity) {
		const float alpha = SvgClamp01(((color >> 24) & 0xff) * INV_255_1 * opacity);
		return D2D1_COLOR_F{
			(color & 0xff) * INV_255_1,
			((color >> 8) & 0xff) * INV_255_1,
			((color >> 16) & 0xff) * INV_255_1,
			alpha
		};
	}

	ComPtr<ID2D1Brush> CreateSvgPaintBrush(const SvgPaint& paint, float opacity, D2DGraphics& graphics) {
		ComPtr<ID2D1Brush> brush;
		switch (paint.type) {
		case SVG_PAINT_NONE:
			return {};
		case SVG_PAINT_COLOR:
			brush.Attach(graphics.CreateSolidColorBrush(SvgColorToD2D(paint.color, opacity)));
			return brush;
		case SVG_PAINT_LINEAR_GRADIENT:
		{
			if (!paint.gradient || paint.gradient->nstops <= 0) {
				return {};
			}
			std::vector<D2D1_GRADIENT_STOP> stops;
			stops.reserve(static_cast<size_t>(paint.gradient->nstops));
			for (int i = 0; i < paint.gradient->nstops; ++i) {
				const auto& stop = paint.gradient->stops[i];
				stops.push_back(D2D1_GRADIENT_STOP{ SvgClamp01(stop.offset), SvgColorToD2D(stop.color, opacity) });
			}
			brush.Attach(graphics.CreateLinearGradientBrush(stops.data(), static_cast<unsigned int>(stops.size())));
			return brush;
		}
		case SVG_PAINT_RADIAL_GRADIENT:
		{
			if (!paint.gradient || paint.gradient->nstops <= 0) {
				return {};
			}
			std::vector<D2D1_GRADIENT_STOP> stops;
			stops.reserve(static_cast<size_t>(paint.gradient->nstops));
			for (int i = 0; i < paint.gradient->nstops; ++i) {
				const auto& stop = paint.gradient->stops[i];
				stops.push_back(D2D1_GRADIENT_STOP{ SvgClamp01(stop.offset), SvgColorToD2D(stop.color, opacity) });
			}
			brush.Attach(graphics.CreateRadialGradientBrush(
				stops.data(),
				static_cast<unsigned int>(stops.size()),
				D2D1::Point2F(paint.gradient->fx, paint.gradient->fy)));
			return brush;
		}
		default:
			return {};
		}
	}
}

std::shared_ptr<BitmapSource> D2DGraphics::ToBitmapFromSvg(const char* svgText, UINT maxBitmapExtent) {
	if (!svgText) {
		return {};
	}
	return ToBitmapFromSvg(std::string_view(svgText), maxBitmapExtent);
}

std::shared_ptr<BitmapSource> D2DGraphics::ToBitmapFromSvg(std::string_view svgText, UINT maxBitmapExtent) {
	if (svgText.empty() || maxBitmapExtent == 0) {
		return {};
	}

	std::vector<char> mutableSvg(svgText.begin(), svgText.end());
	mutableSvg.push_back('\0');

	struct SvgImageDeleter {
		void operator()(SvgImage* image) const noexcept {
			DeleteSvgImageInternal(image);
		}
	};
	std::unique_ptr<SvgImage, SvgImageDeleter> image(ParseSvgImageInternal(mutableSvg.data(), "px", 96.0f));
	if (!image || !std::isfinite(image->width) || !std::isfinite(image->height) || image->width <= 0.0f || image->height <= 0.0f) {
		return {};
	}

	const float largestExtent = (std::max)(image->width, image->height);
	if (largestExtent <= 0.0f || !std::isfinite(largestExtent)) {
		return {};
	}
	const float scale = largestExtent > static_cast<float>(maxBitmapExtent)
		? static_cast<float>(maxBitmapExtent) / largestExtent
		: 1.0f;

	const auto pixelWidth = static_cast<int>((std::max)(1.0f, std::ceil(image->width * scale)));
	const auto pixelHeight = static_cast<int>((std::max)(1.0f, std::ceil(image->height * scale)));
	auto bitmapSource = BitmapSource::CreateEmpty(pixelWidth, pixelHeight);
	if (!bitmapSource) {
		return {};
	}

	D2DGraphics graphics(bitmapSource.get());
	graphics.BeginRender();
	graphics.Clear(D2D1::ColorF(0, 0.0f));

	for (const SvgShape* shape = image->shapes; shape; shape = shape->next) {
		if ((shape->flags & SVG_FLAGS_VISIBLE) == 0) {
			continue;
		}

		ComPtr<ID2D1PathGeometry> geometry;
		geometry.Attach(Factory::CreateGeomtry());
		if (!geometry) {
			continue;
		}

		ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(geometry->Open(&sink)) || !sink) {
			continue;
		}

		bool hasFigure = false;
		for (const SvgPath* path = shape->paths; path; path = path->next) {
			if (!path->pts || path->npts < 4) {
				continue;
			}

			const float* points = path->pts;
			sink->BeginFigure(
				D2D1::Point2F(points[0] * scale, points[1] * scale),
				shape->fill.type == SVG_PAINT_NONE ? D2D1_FIGURE_BEGIN_HOLLOW : D2D1_FIGURE_BEGIN_FILLED);
			hasFigure = true;

			for (int i = 0; i < path->npts - 1; i += 3) {
				const float* p = &path->pts[i * 2];
				sink->AddBezier(D2D1::BezierSegment(
					D2D1::Point2F(p[2] * scale, p[3] * scale),
					D2D1::Point2F(p[4] * scale, p[5] * scale),
					D2D1::Point2F(p[6] * scale, p[7] * scale)));
			}

			sink->EndFigure(path->closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
		}

		if (!hasFigure || FAILED(sink->Close())) {
			continue;
		}

		auto fillBrush = CreateSvgPaintBrush(shape->fill, shape->opacity, graphics);
		if (fillBrush) {
			graphics.FillGeometry(geometry.Get(), fillBrush.Get());
		}

		auto strokeBrush = CreateSvgPaintBrush(shape->stroke, shape->opacity, graphics);
		if (strokeBrush && shape->strokeWidth > 0.0f) {
			graphics.DrawGeometry(geometry.Get(), strokeBrush.Get(), shape->strokeWidth * scale);
		}
	}

	graphics.EndRender();
	return bitmapSource;
}

CompatibleGraphics::CompatibleGraphics(D2DGraphics* parent, D2D1_SIZE_F desiredSize) {
	Initialize(parent, desiredSize);
}

HRESULT CompatibleGraphics::Initialize(D2DGraphics* parent, D2D1_SIZE_F desiredSize) {
	if (!parent) return E_INVALIDARG;
	parent->EnsureDeviceContext();
	auto* pdc = parent->GetDeviceContextRaw();
	if (pdc) {
		pdc->GetDevice(&parentDevice);
	}
	if (!parentDevice) return E_FAIL;
	this->desiredSize = desiredSize;
	return RecreateTarget();
}

HRESULT CompatibleGraphics::RecreateTarget() {
	ResetTarget();
	pD2DDevice = parentDevice;
	if (!pD2DDevice) return E_FAIL;
	HRESULT hr = pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &pDeviceContext);
	if (FAILED(hr)) return hr;

	UINT w = std::max<UINT>(1, static_cast<UINT>(desiredSize.width));
	UINT h = std::max<UINT>(1, static_cast<UINT>(desiredSize.height));

	hr = CreateTargetBitmapForSize(w, h, 96.0f, 96.0f, DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
	if (FAILED(hr)) return hr;

	surfaceKind = SurfaceKind::Compatible;
	wicDirty = false;
	return ConfigDefaultObjects();
}

void CompatibleGraphics::ReSize(UINT width, UINT height) {
	width = std::max<UINT>(1, width);
	height = std::max<UINT>(1, height);
	desiredSize = D2D1::SizeF((FLOAT)width, (FLOAT)height);
	RecreateTarget();
}

ID2D1Bitmap* CompatibleGraphics::GetBitmap() const {
	return pTargetBitmap.Get();
}

HRESULT HwndGraphics::InitDevice() {
	if (!hwnd) return E_INVALIDARG;

	// Acquire the complete device domain in one registry snapshot. Taking the
	// D2D and D3D halves separately could mix generations during device loss.
	ComPtr<ID3D11Device> d3dDevice;
	ComPtr<IDXGIDevice> dxgiDevice;
	ComPtr<ID2D1Device> d2dDevice;
	HRESULT hr = Graphics_AcquireSharedD3DDevice(
		d3dDevice.GetAddressOf(), nullptr, dxgiDevice.GetAddressOf(),
		d2dDevice.GetAddressOf(), nullptr);
	if (FAILED(hr) || !d3dDevice || !dxgiDevice || !d2dDevice)
		return FAILED(hr) ? hr : E_FAIL;

	pD2DDevice = d2dDevice;
	hr = pD2DDevice->CreateDeviceContext(
		D2D1_DEVICE_CONTEXT_OPTIONS_NONE, pDeviceContext.ReleaseAndGetAddressOf());
	if (FAILED(hr)) return hr;

	RECT rc{};
	GetClientRect(hwnd, &rc);
	UINT width = std::max<UINT>(1, static_cast<UINT>(rc.right - rc.left));
	UINT height = std::max<UINT>(1, static_cast<UINT>(rc.bottom - rc.top));

	// CreateSwapChainForHwnd 需要 IDXGIFactory2
	ComPtr<IDXGIAdapter> adapter;
	hr = dxgiDevice->GetAdapter(&adapter);
	if (FAILED(hr)) return hr;
	ComPtr<IDXGIFactory2> factory;
	hr = adapter->GetParent(IID_PPV_ARGS(&factory));
	if (FAILED(hr)) return hr;

	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.Width = width;
	desc.Height = height;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	desc.Scaling = DXGI_SCALING_NONE;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

	ComPtr<IDXGISwapChain1> swapChain1;
	hr = factory->CreateSwapChainForHwnd(d3dDevice.Get(), hwnd, &desc, nullptr, nullptr, &swapChain1);
	if (SUCCEEDED(hr)) {
		factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
		pSwapChain = swapChain1;
	}
	else {
		DXGI_SWAP_CHAIN_DESC legacyDesc{};
		legacyDesc.BufferDesc.Width = width;
		legacyDesc.BufferDesc.Height = height;
		legacyDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		legacyDesc.BufferDesc.RefreshRate.Numerator = 60;
		legacyDesc.BufferDesc.RefreshRate.Denominator = 1;
		legacyDesc.SampleDesc.Count = 1;
		legacyDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		legacyDesc.BufferCount = 2;
		legacyDesc.OutputWindow = hwnd;
		legacyDesc.Windowed = TRUE;
		legacyDesc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;

		ComPtr<IDXGIFactory> legacyFactory;
		hr = adapter->GetParent(IID_PPV_ARGS(&legacyFactory));
		if (FAILED(hr)) return hr;

		ComPtr<IDXGISwapChain> legacySwapChain;
		hr = legacyFactory->CreateSwapChain(d3dDevice.Get(), &legacyDesc, &legacySwapChain);
		if (FAILED(hr)) return hr;

		legacyFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
		pSwapChain = legacySwapChain;
	}

	hr = CreateTargetBitmapForSwapChain(pSwapChain.Get());
	if (FAILED(hr)) return hr;

	surfaceKind = SurfaceKind::Hwnd;
	wicDirty = false;
	return ConfigDefaultObjects();
}

HwndGraphics::HwndGraphics(HWND hWnd) {
	hwnd = hWnd;
	if (FAILED(InitDevice()))
		ResetTarget();
}

void HwndGraphics::ReSize(UINT width, UINT height) {
	if (!pSwapChain || !pDeviceContext) return;
	width = std::max<UINT>(1, width);
	height = std::max<UINT>(1, height);

	pTargetBitmap.Reset();
	pDeviceContext->SetTarget(nullptr);
	pDeviceContext->Flush();

	HRESULT hr = pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
	_lastPresentHr = hr;
	if (FAILED(hr)) {
		_deviceLost = true;
		return;
	}
	_lastEndDrawHr = CreateTargetBitmapForSwapChain(pSwapChain.Get());
	if (FAILED(_lastEndDrawHr)) {
		_deviceLost = true;
		return;
	}
	_swapChainHasPresentedFrame = false;
	(void)ConfigDefaultObjects();
}

void HwndGraphics::BeginRender() {
	D2DGraphics::BeginRender();
}

void HwndGraphics::EndRender() {
	// Hwnd：既需要 EndDraw，也需要 Present
	if (!pDeviceContext) {
		_lastEndDrawHr = E_POINTER;
		return;
	}
	_lastEndDrawHr = pDeviceContext->EndDraw();
	_lastPresentHr = S_OK;
	if (pSwapChain) {
		_lastPresentHr = PresentSwapChain(1, 0);
	}
	if (_lastEndDrawHr == D2DERR_RECREATE_TARGET
		|| IsDeviceRemovedHr(_lastEndDrawHr)
		|| IsDeviceRemovedHr(_lastPresentHr)) {
		_deviceLost = true;
	}
}

// ---------------- CompositionSwapChainGraphics ----------------

CompositionSwapChainGraphics::CompositionSwapChainGraphics(
	IDXGISwapChain1* swapChain,
	UINT presentSyncInterval,
	ID2D1Device* sharedDevice) {
	swapChain1 = swapChain;
	_presentSyncInterval = presentSyncInterval > 4u
		? 4u : presentSyncInterval;
	InitializeWithSwapChain(swapChain, sharedDevice);
}

void CompositionSwapChainGraphics::ReSize(UINT width, UINT height) {
	if (!pSwapChain || !pDeviceContext) return;
	width = std::max<UINT>(1, width);
	height = std::max<UINT>(1, height);

	pTargetBitmap.Reset();
	pDeviceContext->SetTarget(nullptr);
	pDeviceContext->Flush();

	HRESULT hr = pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
	_lastPresentHr = hr;
	if (FAILED(hr)) {
		_deviceLost = true;
		return;
	}
	_lastEndDrawHr = CreateTargetBitmapForSwapChain(pSwapChain.Get());
	if (FAILED(_lastEndDrawHr)) {
		_deviceLost = true;
		return;
	}
	_swapChainHasPresentedFrame = false;
	(void)ConfigDefaultObjects();
}

void CompositionSwapChainGraphics::BeginRender() {
	D2DGraphics::BeginRender();
}

void CompositionSwapChainGraphics::EndRender() {
	if (!pDeviceContext) {
		_lastEndDrawHr = E_POINTER;
		return;
	}
	const GraphicsWorkClock endDrawClock;
	_lastEndDrawHr = pDeviceContext->EndDraw();
	_lastEndDrawMicroseconds = endDrawClock.ElapsedMicroseconds();
	_lastPresentHr = S_OK;
	if (pSwapChain) {
		const GraphicsWorkClock presentClock;
		_lastPresentHr = PresentSwapChain(_presentSyncInterval, 0);
		_lastPresentMicroseconds = presentClock.ElapsedMicroseconds();
	}
	if (_lastEndDrawHr == D2DERR_RECREATE_TARGET
		|| IsDeviceRemovedHr(_lastEndDrawHr)
		|| IsDeviceRemovedHr(_lastPresentHr)) {
		_deviceLost = true;
	}
}
