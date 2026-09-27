#include "PresentationRenderHost.h"

#include "DCompLayeredHost.h"
#include "Graphics.h"

#include <algorithm>
#include <cmath>
#include <d2derr.h>
#include <dcomp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <limits>
#include <unordered_set>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr uint64_t SceneSwapChainBufferCount = 2u;
	constexpr uint64_t SceneBytesPerPixel = 4u;

	class ScopedMicrosecondAccumulator final
	{
	public:
		explicit ScopedMicrosecondAccumulator(double& accumulator) noexcept
			: _accumulator(accumulator)
		{
			(void)::QueryPerformanceCounter(&_start);
		}

		~ScopedMicrosecondAccumulator()
		{
			LARGE_INTEGER end{};
			if (!::QueryPerformanceCounter(&end)) return;
			static const double frequency = []() noexcept
			{
				LARGE_INTEGER value{};
				return ::QueryPerformanceFrequency(&value) && value.QuadPart > 0
					? static_cast<double>(value.QuadPart) : 0.0;
			}();
			if (frequency <= 0.0) return;
			_accumulator += static_cast<double>(
				end.QuadPart - _start.QuadPart) * 1'000'000.0 / frequency;
		}

	private:
		double& _accumulator;
		LARGE_INTEGER _start{};
	};

	class CompositionSurfaceGraphics final : public D2DGraphics
	{
	public:
		struct SubmittedSnapshotStatistics final
		{
			uint64_t CreateCount = 0;
			uint64_t UpdateCount = 0;
			uint64_t CopiedBytes = 0;
			double CreateMicroseconds = 0.0;
			double CopyMicroseconds = 0.0;
		};

		CompositionSurfaceGraphics(
			IDCompositionSurface* surface,
			ID2D1Device* device,
			UINT width,
			UINT height,
			bool useDirect2DUpdateContext)
			: D2DGraphics(device),
			_surface(surface),
			_width((std::max)(UINT{ 1 }, width)),
			_height((std::max)(UINT{ 1 }, height)),
			_useDirect2DUpdateContext(useDirect2DUpdateContext)
		{
			_ownedDeviceContext = pDeviceContext;
			if (_surface) (void)_surface.As(&_virtualSurface);
			surfaceKind = SurfaceKind::CompositionSurface;
			_presentSyncInterval = 0u;
			_updateRect = Bounds();
		}

		~CompositionSurfaceGraphics() override
		{
			if (_directDrawing)
			{
				pDeviceContext = _ownedDeviceContext;
				_updateDeviceContext.Reset();
			}
			if (_drawing && _surface) (void)_surface->EndDraw();
			if (pDeviceContext) pDeviceContext->SetTarget(nullptr);
		}

		bool UsesDirect2DUpdateContext() const noexcept
		{
			return _useDirect2DUpdateContext;
		}

		void SetPresentDirtyRect(const RECT& logicalDirty) override
		{
			FLOAT dpiX = 96.0f;
			FLOAT dpiY = 96.0f;
			if (pDeviceContext) pDeviceContext->GetDpi(&dpiX, &dpiY);
			const float scaleX = dpiX > 0.0f ? dpiX / 96.0f : 1.0f;
			const float scaleY = dpiY > 0.0f ? dpiY / 96.0f : 1.0f;
			RECT physical{
				static_cast<LONG>(std::floor(logicalDirty.left * scaleX)),
				static_cast<LONG>(std::floor(logicalDirty.top * scaleY)),
				static_cast<LONG>(std::ceil(logicalDirty.right * scaleX)),
				static_cast<LONG>(std::ceil(logicalDirty.bottom * scaleY)) };
			const RECT bounds = Bounds();
			RECT clipped{};
			_updateRect = ::IntersectRect(&clipped, &physical, &bounds)
				? clipped : bounds;
			if (_requiresFullFrame) _updateRect = bounds;
		}

		bool RequiresFullPresentFrame() const noexcept override
		{
			return _requiresFullFrame;
		}

		ID3D11Texture2D* GetSubmittedTextureForReadback() const noexcept override
		{
			return _submittedTexture.Get();
		}

		bool CanEnableSubmittedTextureCapture() const noexcept
		{
			return _submittedTextureCaptureEnabled || _requiresFullFrame;
		}

		void SetSubmittedTextureCaptureEnabled(bool enabled) noexcept
		{
			_submittedTextureCaptureEnabled = enabled;
			if (!enabled) _submittedTexture.Reset();
		}

		uint64_t SubmittedTextureBytes() const noexcept
		{
			if (!_submittedTexture) return 0;
			D3D11_TEXTURE2D_DESC description{};
			_submittedTexture->GetDesc(&description);
			return static_cast<uint64_t>(description.Width)
				* description.Height * SceneBytesPerPixel;
		}

		SubmittedSnapshotStatistics SubmittedSnapshotStats() const noexcept
		{
			return _submittedSnapshotStatistics;
		}

		double GetLastSurfaceSubmitMicroseconds() const noexcept override
		{
			return _lastSurfaceSubmitMicroseconds;
		}

		void BeginRender() override
		{
			_lastEndDrawHr = S_OK;
			_lastPresentHr = S_OK;
			_lastEndDrawMicroseconds = 0.0;
			_lastPresentMicroseconds = 0.0;
			_lastSurfaceSubmitMicroseconds = 0.0;
			if (_drawing || !_surface || !pDeviceContext)
			{
				_lastEndDrawHr = E_UNEXPECTED;
				return;
			}
			_updateSurface.Reset();
			_updateDeviceContext.Reset();
			_updateOffset = {};
			if (_useDirect2DUpdateContext)
			{
				HRESULT result = _surface->BeginDraw(
					&_updateRect,
					__uuidof(ID2D1DeviceContext),
					reinterpret_cast<void**>(
						_updateDeviceContext.ReleaseAndGetAddressOf()),
					&_updateOffset);
				if (FAILED(result) || !_updateDeviceContext)
				{
					_lastEndDrawHr = FAILED(result) ? result : E_FAIL;
					if (result == D2DERR_RECREATE_TARGET
						|| result == DXGI_ERROR_DEVICE_REMOVED
						|| result == DXGI_ERROR_DEVICE_RESET) _deviceLost = true;
					return;
				}
				pDeviceContext = _updateDeviceContext;
				FLOAT dpiX = 96.0f;
				FLOAT dpiY = 96.0f;
				if (_ownedDeviceContext)
					_ownedDeviceContext->GetDpi(&dpiX, &dpiY);
				pDeviceContext->SetDpi(dpiX, dpiY);
				if (_submittedTextureCaptureEnabled)
				{
					ComPtr<ID2D1Image> target;
					pDeviceContext->GetTarget(target.GetAddressOf());
					if (!target || FAILED(target.As(&pTargetBitmap))
						|| !pTargetBitmap
						|| FAILED(pTargetBitmap->GetSurface(
							_updateSurface.ReleaseAndGetAddressOf()))
						|| !_updateSurface)
					{
						pTargetBitmap.Reset();
						pDeviceContext = _ownedDeviceContext;
						_updateDeviceContext.Reset();
						(void)_surface->EndDraw();
						_lastEndDrawHr = E_FAIL;
						return;
					}
				}
				const float scaleX = dpiX > 0.0f ? dpiX / 96.0f : 1.0f;
				const float scaleY = dpiY > 0.0f ? dpiY / 96.0f : 1.0f;
				_targetOriginTransform = D2D1::Matrix3x2F::Translation(
					(static_cast<float>(_updateOffset.x - _updateRect.left))
						/ scaleX,
					(static_cast<float>(_updateOffset.y - _updateRect.top))
						/ scaleY);
				_directDrawing = true;
				_drawing = true;
				return;
			}
			HRESULT result = _surface->BeginDraw(
				&_updateRect,
				__uuidof(IDXGISurface),
				reinterpret_cast<void**>(
					_updateSurface.ReleaseAndGetAddressOf()),
				&_updateOffset);
			if (FAILED(result) || !_updateSurface)
			{
				_lastEndDrawHr = FAILED(result) ? result : E_FAIL;
				if (result == D2DERR_RECREATE_TARGET
					|| result == DXGI_ERROR_DEVICE_REMOVED
					|| result == DXGI_ERROR_DEVICE_RESET) _deviceLost = true;
				return;
			}

			D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
				D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
				D2D1::PixelFormat(
					DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
				0.0f, 0.0f);
			result = pDeviceContext->CreateBitmapFromDxgiSurface(
				_updateSurface.Get(), &properties,
				pTargetBitmap.ReleaseAndGetAddressOf());
			if (FAILED(result) || !pTargetBitmap)
			{
				(void)_surface->EndDraw();
				_updateSurface.Reset();
				_lastEndDrawHr = FAILED(result) ? result : E_FAIL;
				return;
			}
			pDeviceContext->SetTarget(pTargetBitmap.Get());
			FLOAT dpiX = 96.0f;
			FLOAT dpiY = 96.0f;
			pDeviceContext->GetDpi(&dpiX, &dpiY);
			const float scaleX = dpiX > 0.0f ? dpiX / 96.0f : 1.0f;
			const float scaleY = dpiY > 0.0f ? dpiY / 96.0f : 1.0f;
			_targetOriginTransform = D2D1::Matrix3x2F::Translation(
				(static_cast<float>(_updateOffset.x - _updateRect.left)) / scaleX,
				(static_cast<float>(_updateOffset.y - _updateRect.top)) / scaleY);
			D2DGraphics::BeginRender();
			_drawing = SUCCEEDED(_lastEndDrawHr);
		}

		void EndRender() override
		{
			if (!_drawing || !_surface || !pDeviceContext)
			{
				_lastEndDrawHr = E_UNEXPECTED;
				return;
			}
			_lastEndDrawMicroseconds = 0.0;
			if (_directDrawing)
				_lastEndDrawHr = S_OK;
			else
			{
				ScopedMicrosecondAccumulator timing(_lastEndDrawMicroseconds);
				_lastEndDrawHr = pDeviceContext->EndDraw();
			}
			if (!_directDrawing && SUCCEEDED(_lastEndDrawHr)
				&& _submittedTextureCaptureEnabled
				&& !CaptureSubmittedTexture())
				_lastEndDrawHr = E_FAIL;
			if (_directDrawing && _submittedTextureCaptureEnabled)
			{
				_lastEndDrawHr = pDeviceContext->Flush();
				if (SUCCEEDED(_lastEndDrawHr)
					&& !CaptureSubmittedTexture())
					_lastEndDrawHr = E_FAIL;
			}
			if (_directDrawing)
			{
				pDeviceContext = _ownedDeviceContext;
				_updateDeviceContext.Reset();
				_directDrawing = false;
			}
			_lastPresentMicroseconds = 0.0;
			_lastSurfaceSubmitMicroseconds = 0.0;
			{
				ScopedMicrosecondAccumulator timing(
					_lastSurfaceSubmitMicroseconds);
				_lastPresentHr = _surface->EndDraw();
			}
			pDeviceContext->SetTarget(nullptr);
			pTargetBitmap.Reset();
			_updateSurface.Reset();
			_targetOriginTransform = D2D1::Matrix3x2F::Identity();
			_drawing = false;
			if (SUCCEEDED(_lastEndDrawHr) && SUCCEEDED(_lastPresentHr))
				_requiresFullFrame = false;
			if (_lastEndDrawHr == D2DERR_RECREATE_TARGET
				|| _lastEndDrawHr == DXGI_ERROR_DEVICE_REMOVED
				|| _lastEndDrawHr == DXGI_ERROR_DEVICE_RESET
				|| _lastPresentHr == DXGI_ERROR_DEVICE_REMOVED
				|| _lastPresentHr == DXGI_ERROR_DEVICE_RESET) _deviceLost = true;
		}

		void ReSize(UINT width, UINT height) override
		{
			width = (std::max)(UINT{ 1 }, width);
			height = (std::max)(UINT{ 1 }, height);
			if (width == _width && height == _height) return;
			if (_drawing || !_virtualSurface)
			{
				_lastPresentHr = E_NOTIMPL;
				_deviceLost = true;
				return;
			}
			_lastPresentHr = _virtualSurface->Resize(width, height);
			if (FAILED(_lastPresentHr))
			{
				_deviceLost = true;
				return;
			}
			_width = width;
			_height = height;
			_submittedTexture.Reset();
			_requiresFullFrame = true;
			_updateRect = Bounds();
		}

	private:
		RECT Bounds() const noexcept
		{
			return RECT{ 0, 0,
				static_cast<LONG>(_width), static_cast<LONG>(_height) };
		}

		bool CaptureSubmittedTexture() noexcept
		{
			ComPtr<ID3D11Texture2D> source;
			if (!_updateSurface || FAILED(_updateSurface.As(&source)) || !source)
				return false;
			D3D11_TEXTURE2D_DESC sourceDescription{};
			source->GetDesc(&sourceDescription);
			ComPtr<ID3D11Device> device;
			source->GetDevice(device.ReleaseAndGetAddressOf());
			if (!device) return false;
			D3D11_TEXTURE2D_DESC submittedDescription{};
			if (_submittedTexture)
				_submittedTexture->GetDesc(&submittedDescription);
			if (!_submittedTexture
				|| submittedDescription.Width != _width
				|| submittedDescription.Height != _height
				|| submittedDescription.Format != sourceDescription.Format)
			{
				D3D11_TEXTURE2D_DESC description{};
				description.Width = _width;
				description.Height = _height;
				description.MipLevels = 1;
				description.ArraySize = 1;
				description.Format = sourceDescription.Format;
				description.SampleDesc.Count = 1;
				description.Usage = D3D11_USAGE_DEFAULT;
				HRESULT result = E_FAIL;
				{
					ScopedMicrosecondAccumulator timing(
						_submittedSnapshotStatistics.CreateMicroseconds);
					result = device->CreateTexture2D(
						&description, nullptr,
						_submittedTexture.ReleaseAndGetAddressOf());
				}
				if (FAILED(result) || !_submittedTexture) return false;
				++_submittedSnapshotStatistics.CreateCount;
			}
			const UINT width = static_cast<UINT>(
				_updateRect.right - _updateRect.left);
			const UINT height = static_cast<UINT>(
				_updateRect.bottom - _updateRect.top);
			if (width == 0 || height == 0
				|| _updateOffset.x < 0 || _updateOffset.y < 0
				|| static_cast<UINT>(_updateOffset.x) + width
					> sourceDescription.Width
				|| static_cast<UINT>(_updateOffset.y) + height
					> sourceDescription.Height) return false;
			ComPtr<ID3D11DeviceContext> context;
			device->GetImmediateContext(context.ReleaseAndGetAddressOf());
			if (!context) return false;
			D3D11_BOX sourceBox{
				static_cast<UINT>(_updateOffset.x),
				static_cast<UINT>(_updateOffset.y), 0u,
				static_cast<UINT>(_updateOffset.x) + width,
				static_cast<UINT>(_updateOffset.y) + height, 1u };
			{
				ScopedMicrosecondAccumulator timing(
					_submittedSnapshotStatistics.CopyMicroseconds);
				context->CopySubresourceRegion(
					_submittedTexture.Get(), 0,
					static_cast<UINT>(_updateRect.left),
					static_cast<UINT>(_updateRect.top), 0,
					source.Get(), 0, &sourceBox);
			}
			++_submittedSnapshotStatistics.UpdateCount;
			_submittedSnapshotStatistics.CopiedBytes +=
				static_cast<uint64_t>(width) * height * SceneBytesPerPixel;
			return true;
		}

		ComPtr<IDCompositionSurface> _surface;
		ComPtr<IDCompositionVirtualSurface> _virtualSurface;
		ComPtr<ID2D1DeviceContext> _ownedDeviceContext;
		ComPtr<ID2D1DeviceContext> _updateDeviceContext;
		ComPtr<IDXGISurface> _updateSurface;
		ComPtr<ID3D11Texture2D> _submittedTexture;
		UINT _width = 1;
		UINT _height = 1;
		RECT _updateRect{ 0, 0, 1, 1 };
		POINT _updateOffset{};
		bool _requiresFullFrame = true;
		bool _drawing = false;
		bool _directDrawing = false;
		bool _submittedTextureCaptureEnabled = false;
		bool _useDirect2DUpdateContext = false;
		SubmittedSnapshotStatistics _submittedSnapshotStatistics;
		double _lastSurfaceSubmitMicroseconds = 0.0;
	};

	uint64_t SaturatingAdd(uint64_t left, uint64_t right) noexcept
	{
		return right > UINT64_MAX - left ? UINT64_MAX : left + right;
	}

	uint64_t SaturatingMultiply(uint64_t left, uint64_t right) noexcept
	{
		return left != 0u && right > UINT64_MAX / left
			? UINT64_MAX : left * right;
	}

	uint64_t EstimatedSwapChainBytes(UINT width, UINT height) noexcept
	{
		if (width == 0 || height == 0) return 0;
		constexpr uint64_t bytesPerPixelAcrossBuffers =
			SceneSwapChainBufferCount * SceneBytesPerPixel;
		const uint64_t pixels = static_cast<uint64_t>(width) * height;
		return pixels > UINT64_MAX / bytesPerPixelAcrossBuffers
			? UINT64_MAX : pixels * bytesPerPixelAcrossBuffers;
	}

	uint64_t EstimatedSurfaceBytes(UINT width, UINT height) noexcept
	{
		if (width == 0 || height == 0) return 0;
		const uint64_t pixels = static_cast<uint64_t>(width) * height;
		return pixels > UINT64_MAX / SceneBytesPerPixel
			? UINT64_MAX : pixels * SceneBytesPerPixel;
	}

	bool IsFiniteMatrix(const D2D1_MATRIX_3X2_F& value) noexcept
	{
		return std::isfinite(value._11) && std::isfinite(value._12)
			&& std::isfinite(value._21) && std::isfinite(value._22)
			&& std::isfinite(value._31) && std::isfinite(value._32);
	}

	bool MatricesEqual(
		const D2D1_MATRIX_3X2_F& left,
		const D2D1_MATRIX_3X2_F& right) noexcept
	{
		return left._11 == right._11 && left._12 == right._12
			&& left._21 == right._21 && left._22 == right._22
			&& left._31 == right._31 && left._32 == right._32;
	}

	bool IsFiniteRect(const D2D1_RECT_F& value) noexcept
	{
		return std::isfinite(value.left) && std::isfinite(value.top)
			&& std::isfinite(value.right) && std::isfinite(value.bottom)
			&& value.right >= value.left && value.bottom >= value.top;
	}

	bool IsFinite(
		const PresentationRenderHost::SceneLayerVisualProperties& value) noexcept
	{
		const auto& transform = value.PhysicalTransform;
		if (!IsFiniteMatrix(transform)
			|| !std::isfinite(value.Opacity)
			|| value.Opacity < 0.0f
			|| value.Opacity > 1.0f
			|| (value.HasClip && !IsFiniteRect(value.PhysicalClip)))
			return false;
		if (value.HasClip && !value.TransformedClipChain.empty()) return false;
		if (value.TransformedClipChain.size() > 64u) return false;
		for (const auto& clip : value.TransformedClipChain)
			if (!IsFiniteRect(clip.PhysicalClip)
				|| !std::isfinite(clip.RadiusX) || clip.RadiusX < 0.0f
				|| !std::isfinite(clip.RadiusY) || clip.RadiusY < 0.0f
				|| !IsFiniteMatrix(clip.LocalToRootPhysical)) return false;
		return true;
	}

	bool IsFinite(
		const PresentationRenderHost::SceneLayerGroupProperties& value) noexcept
	{
		if (!std::isfinite(value.Opacity)
			|| value.Opacity < 0.0f || value.Opacity > 1.0f
			|| (value.HasClip && !IsFiniteRect(value.PhysicalClip)))
			return false;
		if (value.HasClip && !value.TransformedClipChain.empty()) return false;
		if (value.TransformedClipChain.size() > 64u) return false;
		for (const auto& clip : value.TransformedClipChain)
			if (!IsFiniteRect(clip.PhysicalClip)
				|| !std::isfinite(clip.RadiusX) || clip.RadiusX < 0.0f
				|| !std::isfinite(clip.RadiusY) || clip.RadiusY < 0.0f
				|| !IsFiniteMatrix(clip.LocalToRootPhysical)) return false;
		return true;
	}

	bool ResolveClipTransforms(
		std::span<const PresentationRenderHost::SceneLayerTransformedClip> chain,
		const D2D1_MATRIX_3X2_F& publishedTransform,
		D2D1_MATRIX_3X2_F& rootTransform,
		std::vector<D2D1_MATRIX_3X2_F>& intermediateTransforms,
		D2D1_MATRIX_3X2_F& contentTransform)
	{
		rootTransform = D2D1::Matrix3x2F::Identity();
		intermediateTransforms.clear();
		contentTransform = publishedTransform;
		if (chain.empty()) return true;
		rootTransform = chain.front().LocalToRootPhysical;
		intermediateTransforms.reserve(chain.size() - 1u);
		for (size_t index = 1; index < chain.size(); ++index)
		{
			auto inverseParent = D2D1::Matrix3x2F(
				chain[index - 1u].LocalToRootPhysical._11,
				chain[index - 1u].LocalToRootPhysical._12,
				chain[index - 1u].LocalToRootPhysical._21,
				chain[index - 1u].LocalToRootPhysical._22,
				chain[index - 1u].LocalToRootPhysical._31,
				chain[index - 1u].LocalToRootPhysical._32);
			if (!inverseParent.Invert()) return false;
			intermediateTransforms.push_back(D2D1::Matrix3x2F(
				chain[index].LocalToRootPhysical._11,
				chain[index].LocalToRootPhysical._12,
				chain[index].LocalToRootPhysical._21,
				chain[index].LocalToRootPhysical._22,
				chain[index].LocalToRootPhysical._31,
				chain[index].LocalToRootPhysical._32) * inverseParent);
		}
		auto inverseInner = D2D1::Matrix3x2F(
			chain.back().LocalToRootPhysical._11,
			chain.back().LocalToRootPhysical._12,
			chain.back().LocalToRootPhysical._21,
			chain.back().LocalToRootPhysical._22,
			chain.back().LocalToRootPhysical._31,
			chain.back().LocalToRootPhysical._32);
		if (!inverseInner.Invert()) return false;
		contentTransform = D2D1::Matrix3x2F(
			publishedTransform._11,
			publishedTransform._12,
			publishedTransform._21,
			publishedTransform._22,
			publishedTransform._31,
			publishedTransform._32) * inverseInner;
		return true;
	}

	HRESULT SetRectangleClip(
		IDCompositionRectangleClip* clip,
		const D2D1_RECT_F& value,
		float radiusX = 0.0f,
		float radiusY = 0.0f) noexcept
	{
		if (!clip) return E_POINTER;
		HRESULT result = clip->SetLeft(value.left);
		if (SUCCEEDED(result)) result = clip->SetTop(value.top);
		if (SUCCEEDED(result)) result = clip->SetRight(value.right);
		if (SUCCEEDED(result)) result = clip->SetBottom(value.bottom);
		if (SUCCEEDED(result)) result = clip->SetTopLeftRadiusX(radiusX);
		if (SUCCEEDED(result)) result = clip->SetTopLeftRadiusY(radiusY);
		if (SUCCEEDED(result)) result = clip->SetTopRightRadiusX(radiusX);
		if (SUCCEEDED(result)) result = clip->SetTopRightRadiusY(radiusY);
		if (SUCCEEDED(result)) result = clip->SetBottomRightRadiusX(radiusX);
		if (SUCCEEDED(result)) result = clip->SetBottomRightRadiusY(radiusY);
		if (SUCCEEDED(result)) result = clip->SetBottomLeftRadiusX(radiusX);
		if (SUCCEEDED(result)) result = clip->SetBottomLeftRadiusY(radiusY);
		return result;
	}
}

std::atomic<bool>
	PresentationRenderHost::_failNextPrimaryAttachForTesting{ false };
std::atomic<uint64_t>
	PresentationRenderHost::_failSceneLayerAllocationAfterForTesting{
		UINT64_MAX };
std::atomic<bool>
	PresentationRenderHost::_failNextSceneLayerGroupTopologyStageForTesting{
		false };
std::atomic<bool>
	PresentationRenderHost::_useSceneCompositionSurfaces{ true };

PresentationRenderHost::PresentationRenderHost() = default;

PresentationRenderHost::~PresentationRenderHost()
{
	Detach();
}

bool PresentationRenderHost::Attach(HWND window, UINT dpi)
{
	Detach();
	if (!window || !::IsWindow(window)) return false;

	_window = window;
	_dpi = dpi == 0 ? 96 : dpi;
	RECT client{};
	::GetClientRect(_window, &client);
	_width = static_cast<UINT>((std::max)(
		LONG{ 1 }, client.right - client.left));
	_height = static_cast<UINT>((std::max)(
		LONG{ 1 }, client.bottom - client.top));
	if (_failNextPrimaryAttachForTesting.exchange(
		false, std::memory_order_acq_rel))
	{
		_deviceResetRequested = true;
		InvalidateFrameHistory();
		return false;
	}
	_primary = std::make_unique<HwndGraphics>(_window);
	if (!_primary || !_primary->GetDeviceContextRaw())
	{
		_primary.reset();
		// Preserve the HWND attachment so queued damage can enter RecoverDevice
		// on the next paint after a transient graphics initialization failure.
		_deviceResetRequested = true;
		InvalidateFrameHistory();
		return false;
	}
	GraphicsSharedD3DDeviceInfo deviceInfo{};
	if (SUCCEEDED(Graphics_AcquireSharedD3DDevice(
		nullptr, nullptr, nullptr, nullptr, &deviceInfo)))
	{
		_sharedDeviceGeneration = deviceInfo.Generation;
	}
	_primary->SetDpi(static_cast<FLOAT>(_dpi), static_cast<FLOAT>(_dpi));
	// The device context is owned for the host lifetime, but it is exposed to
	// native render hooks only while a frame surface is open.
	_active = nullptr;
	AdvanceResourceGeneration();
	AdvanceDeviceResourceGeneration();
	InvalidateFrameHistory();
	return true;
}

void PresentationRenderHost::Detach() noexcept
{
	for (auto iterator = _openContexts.rbegin();
		iterator != _openContexts.rend(); ++iterator)
	{
		if (!iterator->Context) continue;
		if (iterator->CommandRecording)
		{
			iterator->Context->AbortCommandRecording();
			continue;
		}
		iterator->Context->ClearTransform();
		if (iterator->ClipPushed) iterator->Context->PopDrawRect();
		iterator->Context->EndRender();
	}
	_openContexts.clear();
	_active = nullptr;
	_overlay.reset();
	ReleaseSceneLayers();
	_primary.reset();
	_composition.reset();
	_window = nullptr;
	_width = 1;
	_height = 1;
	_dpi = 96;
	_recoveringDevice = false;
	_deviceResetRequested = false;
	_hasPresentedFrame = false;
	_transactionOpen = false;
	_activeTransactionSequence = 0;
	_sharedDeviceGeneration = 0;
	_pendingDamage = {};
	_hasPendingDamage = false;
	_fullDamagePending = false;
	_lastPrimaryDirty = {};
	_lastPrimaryWasFull = false;
	_hasLastPrimaryFrame = false;
	_lastSurfaceFailureSequence = 0;
	_lastFailedSurfaceRole = SurfaceRole::Primary;
	_lastFailedEndDrawHr = S_OK;
	_lastFailedPresentHr = S_OK;
	_peakSceneLayerCount = 0;
	_peakSceneLayerSlotCount = 0;
	_peakEstimatedSceneSwapChainBytes = 0;
	_peakEstimatedSceneCompositionSurfaceBytes = 0;
	_peakEstimatedSceneSubmittedSnapshotBytes = 0;
	_peakEstimatedSceneLayerSlotBytes = 0;
	_sceneLayerCreateCount = 0;
	_sceneLayerResizeCount = 0;
	_sceneLayerReleaseCount = 0;
	_sceneLayerAllocationFailureCount = 0;
	_sceneLayerPixelReadbackLeaseCount = 0;
	_sceneLayerSubmittedSnapshotCreateCount = 0;
	_sceneLayerSubmittedSnapshotUpdateCount = 0;
	_sceneLayerSubmittedSnapshotCopiedBytes = 0;
	_sceneLayerSubmittedSnapshotCreateMicroseconds = 0.0;
	_sceneLayerSubmittedSnapshotCopyMicroseconds = 0.0;
	_sceneLayerSlotEnsureMicroseconds = 0.0;
	_sceneLayerTopologyBatchBeginMicroseconds = 0.0;
	_sceneLayerSwapChainCreateMicroseconds = 0.0;
	_sceneLayerCompositionSurfaceCreateMicroseconds = 0.0;
	_sceneLayerVisualCreateMicroseconds = 0.0;
	_sceneLayerVisualBindMicroseconds = 0.0;
	_sceneLayerGraphicsCreateMicroseconds = 0.0;
	_sceneLayerRecorderCreateMicroseconds = 0.0;
	_sceneLayerDpiSetupMicroseconds = 0.0;
	_sceneLayerVisualPropertyStageMicroseconds = 0.0;
	_sceneLayerGroupStageMicroseconds = 0.0;
	_sceneLayerResourcePeakUpdateMicroseconds = 0.0;
	_sceneLayerTopologyBatchCommitMicroseconds = 0.0;
	_sceneLayerTopologyBatchActive = false;
	_sceneLayerTopologyBatchInitialCount = 0;
	_sceneLayerBatchResourceMutationPending = false;
}

bool PresentationRenderHost::IsAttached() const noexcept
{
	return _window && ::IsWindow(_window) && _primary
		&& _primary->GetDeviceContextRaw();
}

void PresentationRenderHost::FailNextPrimaryAttachForTesting() noexcept
{
	_failNextPrimaryAttachForTesting.store(true, std::memory_order_release);
}

void PresentationRenderHost::FailSceneLayerAllocationAfterForTesting(
	size_t successfulCreates) noexcept
{
	_failSceneLayerAllocationAfterForTesting.store(
		static_cast<uint64_t>(successfulCreates), std::memory_order_release);
}

void PresentationRenderHost::ClearSceneLayerAllocationFailureForTesting() noexcept
{
	_failSceneLayerAllocationAfterForTesting.store(
		UINT64_MAX, std::memory_order_release);
}

bool PresentationRenderHost::OwnsContext(
	const D2DGraphics* context) const noexcept
{
	if (!context) return false;
	if (context == _primary.get() || context == _overlay.get()) return true;
	for (const auto& layer : _sceneLayers)
	{
		if (context == layer.Graphics.get()
			|| context == layer.Recorder) return true;
	}
	return false;
}

bool PresentationRenderHost::Activate(D2DGraphics* context) noexcept
{
	if (!context || !OwnsContext(context)) return false;
	_active = context;
	return true;
}

void PresentationRenderHost::Resize(UINT width, UINT height)
{
	width = (std::max)(UINT{ 1 }, width);
	height = (std::max)(UINT{ 1 }, height);
	if (width == _width && height == _height) return;
	_width = width;
	_height = height;
	if (_transactionOpen)
	{
		_deviceResetRequested = true;
		InvalidateFrameHistory();
		return;
	}
	if (_composition) _composition->UpdateD2DLayerSize(_width, _height);
	if (_primary) _primary->ReSize(_width, _height);
	if (_overlay) _overlay->ReSize(_width, _height);
	for (auto& layer : _sceneLayers)
	{
		if (layer.Graphics && layer.SurfaceProperties.FullWindow)
		{
			layer.Graphics->ReSize(_width, _height);
			layer.SurfaceProperties.PhysicalWidth = _width;
			layer.SurfaceProperties.PhysicalHeight = _height;
		}
	}
	SetDpi(_dpi);
	AdvanceResourceGeneration();
	if (IsDeviceLost()) _deviceResetRequested = true;
	InvalidateFrameHistory();
}

void PresentationRenderHost::ResizeToClient()
{
	if (!_window || !::IsWindow(_window)) return;
	RECT client{};
	::GetClientRect(_window, &client);
	Resize(
		static_cast<UINT>((std::max)(
			LONG{ 1 }, client.right - client.left)),
		static_cast<UINT>((std::max)(
			LONG{ 1 }, client.bottom - client.top)));
}

void PresentationRenderHost::SetDpi(UINT dpi)
{
	const UINT value = dpi == 0 ? 96 : dpi;
	const bool changed = value != _dpi;
	_dpi = value;
	const auto nativeDpi = static_cast<FLOAT>(_dpi);
	if (_primary) _primary->SetDpi(nativeDpi, nativeDpi);
	if (_overlay) _overlay->SetDpi(nativeDpi, nativeDpi);
	if (_sceneCommandRecorder)
		_sceneCommandRecorder->SetDpi(nativeDpi, nativeDpi);
	for (auto& layer : _sceneLayers)
	{
		if (layer.Graphics) layer.Graphics->SetDpi(nativeDpi, nativeDpi);
	}
	if (changed)
	{
		AdvanceResourceGeneration();
		InvalidateFrameHistory();
	}
}

RECT PresentationRenderHost::ToLogicalRect(
	const RECT& physical) const noexcept
{
	const float scale = _dpi > 0 ? (_dpi / 96.0f) : 1.0f;
	return RECT{
		static_cast<LONG>(std::floor(physical.left / scale)),
		static_cast<LONG>(std::floor(physical.top / scale)),
		static_cast<LONG>(std::ceil(physical.right / scale)),
		static_cast<LONG>(std::ceil(physical.bottom / scale)) };
}

bool PresentationRenderHost::IsTransactionActive(
	const FrameTransaction& transaction) const noexcept
{
	return _transactionOpen && transaction.Open
		&& transaction.Sequence != 0
		&& transaction.Sequence == _activeTransactionSequence;
}

bool PresentationRenderHost::OpenSurface(
	FrameTransaction& transaction,
	D2DGraphics* context,
	SurfaceRole role,
	const RECT& logicalDirty,
	const RECT& logicalSurfaceClient,
	bool clearTransparent,
	SurfaceFrame& surface)
{
	surface = {};
	if (!IsTransactionActive(transaction) || transaction.Failed
		|| !context || !context->GetDeviceContextRaw()
		|| logicalDirty.right <= logicalDirty.left
		|| logicalDirty.bottom <= logicalDirty.top
		|| logicalSurfaceClient.right <= logicalSurfaceClient.left
		|| logicalSurfaceClient.bottom <= logicalSurfaceClient.top) return false;
	for (const auto& open : _openContexts)
		if (open.Context == context) return false;
	if (!Activate(context)) return false;

	const RECT effectiveDirty = context->RequiresFullPresentFrame()
		? logicalSurfaceClient : logicalDirty;
	context->SetPresentDirtyRect(effectiveDirty);
	context->BeginRender();
	if (FAILED(context->GetLastEndDrawHr()))
	{
		MarkTransactionFailed(transaction);
		return false;
	}
	context->ClearTransform();
	context->PushDrawRect(
		static_cast<float>(effectiveDirty.left),
		static_cast<float>(effectiveDirty.top),
		static_cast<float>(effectiveDirty.right - effectiveDirty.left),
		static_cast<float>(effectiveDirty.bottom - effectiveDirty.top));
	if (clearTransparent)
		context->Clear(D2D1_COLOR_F{ 0.0f, 0.0f, 0.0f, 0.0f });
	_openContexts.push_back(OpenContext{
		context, role, false, true });
	surface.Context = context;
	surface.Role = role;
	surface.LogicalDirty = effectiveDirty;
	surface.Open = true;
	return true;
}

bool PresentationRenderHost::BeginFrameTransaction(
	const RECT& physicalDirty,
	bool force,
	FrameTransaction& transaction)
{
	transaction = {};
	if (_transactionOpen || !IsAttached() || IsDeviceLost()
		|| physicalDirty.right <= physicalDirty.left
		|| physicalDirty.bottom <= physicalDirty.top) return false;

	RECT physicalClient{};
	::GetClientRect(_window, &physicalClient);
	if (++_nextFrameSequence == 0) ++_nextFrameSequence;
	transaction.Sequence = _nextFrameSequence;
	transaction.ResourceGeneration = _resourceGeneration;
	transaction.LogicalClient = ToLogicalRect(physicalClient);
	transaction.FullFrame = NeedsFullFrame(force);
	transaction.LogicalDirty = transaction.FullFrame
		? transaction.LogicalClient : ToLogicalRect(physicalDirty);
	if (!transaction.FullFrame)
	{
		RECT clipped{};
		if (!::IntersectRect(&clipped, &transaction.LogicalDirty,
			&transaction.LogicalClient)) return false;
		transaction.LogicalDirty = clipped;
	}
	transaction.DpiScale = _dpi > 0 ? (_dpi / 96.0f) : 1.0f;
	transaction.Open = true;
	_transactionOpen = true;
	_activeTransactionSequence = transaction.Sequence;
	if (!OpenSurface(transaction, _primary.get(), SurfaceRole::Primary,
		transaction.LogicalDirty, transaction.LogicalClient,
		false, transaction.Primary))
	{
		AbortFrameTransaction(transaction);
		return false;
	}
	return true;
}

bool PresentationRenderHost::OpenSceneSurface(
	FrameTransaction& transaction,
	D2DGraphics* context,
	const RECT& logicalDirty,
	const RECT& logicalSurfaceClient,
	SurfaceFrame& surface)
{
	return OpenSurface(transaction, context, SurfaceRole::Scene,
		logicalDirty, logicalSurfaceClient, true, surface);
}

bool PresentationRenderHost::OpenOverlaySurface(
	FrameTransaction& transaction,
	SurfaceFrame& surface)
{
	if (!_overlay) return false;
	return OpenSurface(transaction, _overlay.get(), SurfaceRole::Overlay,
		transaction.LogicalDirty, transaction.LogicalClient,
		true, surface);
}

bool PresentationRenderHost::ValidateClosedContext(
	D2DGraphics& context) noexcept
{
	return SUCCEEDED(context.GetLastEndDrawHr())
		&& SUCCEEDED(context.GetLastPresentHr())
		&& !context.IsDeviceLost();
}

void PresentationRenderHost::RemoveOpenContext(
	D2DGraphics* context,
	bool commandRecording) noexcept
{
	const auto found = std::find_if(_openContexts.rbegin(),
		_openContexts.rend(), [=](const OpenContext& open)
		{
			return open.Context == context
				&& open.CommandRecording == commandRecording;
		});
	if (found == _openContexts.rend()) return;
	_openContexts.erase(std::next(found).base());
}

void PresentationRenderHost::MarkTransactionFailed(
	FrameTransaction& transaction) noexcept
{
	transaction.Failed = true;
	_deviceResetRequested = true;
	InvalidateFrameHistory();
}

bool PresentationRenderHost::CloseSurface(
	FrameTransaction& transaction,
	SurfaceFrame& surface) noexcept
{
	if (!surface.Open || !surface.Context
		|| !IsTransactionActive(transaction)) return false;
	auto* context = surface.Context;
	context->ClearTransform();
	context->PopDrawRect();
	context->EndRender();
	surface.EndDrawMicroseconds = context->GetLastEndDrawMicroseconds();
	surface.PresentMicroseconds = context->GetLastPresentMicroseconds();
	surface.SurfaceSubmitMicroseconds =
		context->GetLastSurfaceSubmitMicroseconds();
	RemoveOpenContext(context, false);
	surface.Open = false;
	const bool succeeded = ValidateClosedContext(*context);
	if (succeeded && surface.Role == SurfaceRole::Scene
		&& _sceneLayerPixelReadbackLeaseCount > 0u)
		UpdateSceneResourcePeaks();
	if (!succeeded)
	{
		_lastSurfaceFailureSequence = transaction.Sequence;
		_lastFailedSurfaceRole = surface.Role;
		_lastFailedEndDrawHr = context->GetLastEndDrawHr();
		_lastFailedPresentHr = context->GetLastPresentHr();
		MarkTransactionFailed(transaction);
	}
	_active = _openContexts.empty()
		? nullptr : _openContexts.back().Context;
	return succeeded;
}

D2DGraphics* PresentationRenderHost::FindCommandRecorder(
	D2DGraphics* presentationContext) noexcept
{
	for (auto& layer : _sceneLayers)
		if (layer.Graphics.get() == presentationContext)
			return layer.Recorder;
	return nullptr;
}

bool PresentationRenderHost::RecordDrawingCommands(
	FrameTransaction& transaction,
	D2DGraphics* presentationContext,
	const std::function<void()>& draw,
	ID2D1CommandList** commandList) noexcept
{
	if (commandList) *commandList = nullptr;
	if (!commandList || !draw || !IsTransactionActive(transaction)
		|| transaction.Failed) return false;
	auto* recorder = FindCommandRecorder(presentationContext);
	if (!recorder || !recorder->BeginCommandRecording())
	{
		MarkTransactionFailed(transaction);
		return false;
	}
	auto* previous = _active;
	_openContexts.push_back(OpenContext{
		recorder, SurfaceRole::Scene, true, false });
	(void)Activate(recorder);
	try
	{
		draw();
	}
	catch (...)
	{
		recorder->AbortCommandRecording();
		RemoveOpenContext(recorder, true);
		(void)Activate(previous);
		MarkTransactionFailed(transaction);
		return false;
	}
	const HRESULT result = recorder->EndCommandRecording(commandList);
	RemoveOpenContext(recorder, true);
	(void)Activate(previous);
	if (FAILED(result) || !*commandList)
	{
		MarkTransactionFailed(transaction);
		return false;
	}
	return true;
}

bool PresentationRenderHost::ReplayDrawingCommands(
	FrameTransaction& transaction,
	D2DGraphics* presentationContext,
	ID2D1CommandList* commandList,
	D2D1_POINT_2F offset) noexcept
{
	if (!IsTransactionActive(transaction) || transaction.Failed
		|| !presentationContext || !commandList) return false;
	auto* context = presentationContext->GetDeviceContextRaw();
	if (!context || (offset.x == 0.0f && offset.y == 0.0f))
	{
		presentationContext->DrawCommandList(commandList);
		return true;
	}
	// A translated scene node replays its retained list displaced in render
	// space. The list carries its recorded world transform, so applying the
	// translation before the current target transform moves every primitive
	// and recorded clip rigidly. Dropping the offset would present the node at
	// its stale recorded position while its geometry reports the new one.
	D2D1_MATRIX_3X2_F current{};
	context->GetTransform(&current);
	const auto displaced = D2D1::Matrix3x2F::Translation(offset.x, offset.y)
		* D2D1::Matrix3x2F(current._11, current._12, current._21,
			current._22, current._31, current._32);
	context->SetTransform(displaced);
	presentationContext->DrawCommandList(commandList);
	context->SetTransform(current);
	return true;
}

bool PresentationRenderHost::CommitFrameTransaction(
	FrameTransaction& transaction) noexcept
{
	if (!IsTransactionActive(transaction)) return false;
	if (transaction.Primary.Open)
		(void)CloseSurface(transaction, transaction.Primary);
	if (!_openContexts.empty()) MarkTransactionFailed(transaction);
	if (!transaction.Failed && !IsDeviceLost() && _composition)
	{
		const HRESULT result = _composition->CommitComposition();
		if (FAILED(result)) MarkTransactionFailed(transaction);
	}
	if (transaction.Failed || IsDeviceLost())
	{
		AbortFrameTransaction(transaction);
		return false;
	}

	_lastPrimaryDirty = transaction.LogicalDirty;
	_lastPrimaryWasFull = transaction.FullFrame;
	_hasLastPrimaryFrame = true;
	_hasPresentedFrame = true;
	++_committedFrames;
	transaction.Open = false;
	_transactionOpen = false;
	_activeTransactionSequence = 0;
	_active = nullptr;
	return true;
}

void PresentationRenderHost::AbortFrameTransaction(
	FrameTransaction& transaction) noexcept
{
	if (!IsTransactionActive(transaction))
	{
		transaction.Open = false;
		transaction.Failed = true;
		return;
	}
	for (auto iterator = _openContexts.rbegin();
		iterator != _openContexts.rend(); ++iterator)
	{
		auto* context = iterator->Context;
		if (!context) continue;
		if (iterator->CommandRecording)
		{
			context->AbortCommandRecording();
			continue;
		}
		context->ClearTransform();
		if (iterator->ClipPushed) context->PopDrawRect();
		context->EndRender();
	}
	_openContexts.clear();
	transaction.Primary.Open = false;
	transaction.Open = false;
	transaction.Failed = true;
	_transactionOpen = false;
	_activeTransactionSequence = 0;
	_deviceResetRequested = true;
	++_abortedFrames;
	InvalidateFrameHistory();
	_active = nullptr;
}

void PresentationRenderHost::QueueDamage(
	const RECT& physicalDirty) noexcept
{
	if (physicalDirty.right <= physicalDirty.left
		|| physicalDirty.bottom <= physicalDirty.top) return;
	RECT client{ 0, 0, static_cast<LONG>(_width),
		static_cast<LONG>(_height) };
	RECT clipped{};
	if (!::IntersectRect(&clipped, &physicalDirty, &client)) return;
	if (_fullDamagePending)
	{
		// WM_PAINT may already have consumed the HWND update region while a
		// nested layout/render callback promoted retained damage back to full.
		// Re-establish the OS half of the scheduling contract instead of
		// leaving full retained work stranded until unrelated input arrives.
		if (_window && ::IsWindow(_window))
			::InvalidateRect(_window, nullptr, FALSE);
		return;
	}
	if (!_hasPendingDamage)
	{
		_pendingDamage = clipped;
		_hasPendingDamage = true;
		if (_window && ::IsWindow(_window))
			::InvalidateRect(_window, &clipped, FALSE);
		return;
	}
	RECT combined{};
	::UnionRect(&combined, &_pendingDamage, &clipped);
	_pendingDamage = combined;
	if (_window && ::IsWindow(_window))
		::InvalidateRect(_window, &clipped, FALSE);
}

void PresentationRenderHost::QueueFullDamage() noexcept
{
	_pendingDamage = RECT{
		0, 0, static_cast<LONG>(_width), static_cast<LONG>(_height) };
	_hasPendingDamage = true;
	_fullDamagePending = true;
	// The retained damage queue and the HWND update region are one scheduling
	// contract. Internal scene/device invalidations must never leave work queued
	// until an unrelated input or animation happens to produce WM_PAINT.
	if (_window && ::IsWindow(_window))
		::InvalidateRect(_window, nullptr, FALSE);
}

bool PresentationRenderHost::TakePendingDamage(
	RECT& physicalDirty) noexcept
{
	physicalDirty = {};
	if (!_hasPendingDamage) return false;
	physicalDirty = _fullDamagePending
		? RECT{ 0, 0, static_cast<LONG>(_width),
			static_cast<LONG>(_height) }
		: _pendingDamage;
	_pendingDamage = {};
	_hasPendingDamage = false;
	_fullDamagePending = false;
	return physicalDirty.right > physicalDirty.left
		&& physicalDirty.bottom > physicalDirty.top;
}

bool PresentationRenderHost::TryGetLastPrimaryFrame(
	RECT& logicalDirty,
	bool& fullFrame) const noexcept
{
	logicalDirty = _lastPrimaryDirty;
	fullFrame = _lastPrimaryWasFull;
	return _hasLastPrimaryFrame;
}

void PresentationRenderHost::InvalidateFrameHistory() noexcept
{
	_hasPresentedFrame = false;
	_hasLastPrimaryFrame = false;
	QueueFullDamage();
}

bool PresentationRenderHost::IsDeviceLost() const noexcept
{
	if (_deviceResetRequested) return true;
	if (_sharedDeviceGeneration != 0
		&& Graphics_GetSharedD3DDeviceGeneration()
			!= _sharedDeviceGeneration) return true;
	if (_composition && _composition->IsDeviceLost()) return true;
	if (_primary && _primary->IsDeviceLost()) return true;
	if (_overlay && _overlay->IsDeviceLost()) return true;
	if (_sceneCommandRecorder && _sceneCommandRecorder->IsDeviceLost())
		return true;
	for (const auto& layer : _sceneLayers)
	{
		if (layer.Graphics && layer.Graphics->IsDeviceLost()) return true;
	}
	return false;
}

bool PresentationRenderHost::RecoverDevice()
{
	if (_recoveringDevice || _transactionOpen || !_window
		|| !::IsWindow(_window) || !IsDeviceLost()) return false;
	_recoveringDevice = true;
	const bool restoreComposition = UsesComposition();

	_active = nullptr;
	_overlay.reset();
	ReleaseSceneLayers();
	_primary.reset();
	_composition.reset();
	_primary = std::make_unique<HwndGraphics>(_window);
	bool restored = _primary && _primary->GetDeviceContextRaw();
	if (restored)
	{
		_primary->SetDpi(static_cast<FLOAT>(_dpi), static_cast<FLOAT>(_dpi));
		_active = nullptr;
		if (restoreComposition) restored = CreateCompositionResources();
	}
	if (restored)
	{
		GraphicsSharedD3DDeviceInfo deviceInfo{};
		restored = SUCCEEDED(Graphics_AcquireSharedD3DDevice(
			nullptr, nullptr, nullptr, nullptr, &deviceInfo))
			&& deviceInfo.Generation != 0;
		if (restored)
			_sharedDeviceGeneration = deviceInfo.Generation;
	}
	if (restored)
	{
		_deviceResetRequested = false;
		AdvanceResourceGeneration();
		AdvanceDeviceResourceGeneration();
		++_deviceRecoveries;
		InvalidateFrameHistory();
	}
	else
	{
		_deviceResetRequested = true;
	}
	_recoveringDevice = false;
	return restored;
}

void PresentationRenderHost::InjectDeviceLossForTesting() noexcept
{
	++_injectedDeviceLosses;
	_deviceResetRequested = true;
	InvalidateFrameHistory();
}

PresentationRenderHost::TransactionStatistics
PresentationRenderHost::Statistics() const noexcept
{
	return TransactionStatistics{
		_nextFrameSequence,
		_resourceGeneration,
		_committedFrames,
		_abortedFrames,
		_deviceRecoveries,
		_injectedDeviceLosses,
		_lastSurfaceFailureSequence,
		_lastFailedSurfaceRole,
		_lastFailedEndDrawHr,
		_lastFailedPresentHr };
}

PresentationRenderHost::ResourceSnapshot
PresentationRenderHost::Resources() const noexcept
{
	ResourceSnapshot result;
	GraphicsSharedD3DDeviceInfo deviceInfo{};
	ComPtr<IDXGIDevice> dxgiDevice;
	if (SUCCEEDED(Graphics_AcquireSharedD3DDevice(
		nullptr, nullptr, dxgiDevice.GetAddressOf(), nullptr, &deviceInfo)))
	{
		result.DeviceGeneration = deviceInfo.Generation;
		result.IsHardwareAdapter = deviceInfo.IsHardware;
		result.IsSoftwareAdapter = deviceInfo.IsSoftwareAdapter;
		result.SupportsVideo = deviceInfo.SupportsVideo;
		result.FeatureLevel = deviceInfo.FeatureLevel;
		result.VendorId = deviceInfo.VendorId;
		result.DeviceId = deviceInfo.DeviceId;
		result.AdapterLuid = deviceInfo.AdapterLuid;
		result.DedicatedVideoMemoryBytes =
			deviceInfo.DedicatedVideoMemoryBytes;
		result.SharedSystemMemoryBytes = deviceInfo.SharedSystemMemoryBytes;
		(void)::wcsncpy_s(
			result.AdapterDescription,
			deviceInfo.AdapterDescription, _TRUNCATE);
		ComPtr<IDXGIAdapter> adapter;
		ComPtr<IDXGIAdapter3> adapter3;
		if (dxgiDevice
			&& SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))
			&& adapter && SUCCEEDED(adapter.As(&adapter3)) && adapter3)
		{
			DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
			if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
				0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
			{
				result.LocalMemoryBudgetBytes = memory.Budget;
				result.LocalMemoryCurrentUsageBytes = memory.CurrentUsage;
			}
			memory = {};
			if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
				0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &memory)))
			{
				result.NonLocalMemoryBudgetBytes = memory.Budget;
				result.NonLocalMemoryCurrentUsageBytes = memory.CurrentUsage;
			}
		}
	}
	result.SceneLayerCount = SceneLayerCount();
	result.SceneLayerSlotCount = _sceneLayers.size();
	result.SceneLayerSlotCapacity = _sceneLayers.capacity();
	result.SceneLayerGroupCount = _sceneLayerGroups.size();
	result.SceneLayerPixelReadbackLeaseCount =
		_sceneLayerPixelReadbackLeaseCount;
	result.SceneLayerSubmittedSnapshotCreateCount =
		_sceneLayerSubmittedSnapshotCreateCount;
	result.SceneLayerSubmittedSnapshotUpdateCount =
		_sceneLayerSubmittedSnapshotUpdateCount;
	result.SceneLayerSubmittedSnapshotCopiedBytes =
		_sceneLayerSubmittedSnapshotCopiedBytes;
	result.SceneLayerSubmittedSnapshotCreateMicroseconds =
		_sceneLayerSubmittedSnapshotCreateMicroseconds;
	result.SceneLayerSubmittedSnapshotCopyMicroseconds =
		_sceneLayerSubmittedSnapshotCopyMicroseconds;
	bool hasScenePresentInterval = false;
	std::unordered_set<ID2D1Device*> sceneGraphicsDevices;
	std::unordered_set<ID2D1Device*> sceneRecorderDevices;
	auto* sharedSceneDevice = _composition
		? _composition->GetD2DDevice() : nullptr;
	for (const auto& layer : _sceneLayers)
	{
		if (!layer.Visual || !layer.Graphics) continue;
		auto* graphicsDevice = layer.Graphics->GetDeviceRaw();
		if (graphicsDevice)
		{
			sceneGraphicsDevices.insert(graphicsDevice);
			if (graphicsDevice == sharedSceneDevice)
				++result.SceneLayerSharedGraphicsDeviceCount;
		}
		auto* recorderDevice = layer.Recorder
			? layer.Recorder->GetDeviceRaw() : nullptr;
		if (recorderDevice)
		{
			++result.SceneCommandRecorderReferenceCount;
			sceneRecorderDevices.insert(recorderDevice);
			if (recorderDevice == sharedSceneDevice)
				++result.SceneLayerSharedRecorderDeviceCount;
		}
		const UINT presentInterval = layer.Graphics->GetPresentSyncInterval();
		if (!hasScenePresentInterval)
		{
			result.MinimumScenePresentSyncInterval = presentInterval;
			result.MaximumScenePresentSyncInterval = presentInterval;
			hasScenePresentInterval = true;
		}
		else
		{
			result.MinimumScenePresentSyncInterval = (std::min)(
				result.MinimumScenePresentSyncInterval, presentInterval);
			result.MaximumScenePresentSyncInterval = (std::max)(
				result.MaximumScenePresentSyncInterval, presentInterval);
		}
		if (layer.SurfaceProperties.FullWindow)
			++result.FullWindowSceneLayerCount;
		result.MaximumSceneSurfaceWidth = (std::max)(
			result.MaximumSceneSurfaceWidth,
			layer.SurfaceProperties.PhysicalWidth);
		result.MaximumSceneSurfaceHeight = (std::max)(
			result.MaximumSceneSurfaceHeight,
			layer.SurfaceProperties.PhysicalHeight);
		if (layer.UsesCompositionSurface)
		{
			++result.SceneLayerCompositionSurfaceCount;
			const uint64_t bytes = EstimatedSurfaceBytes(
				layer.SurfaceProperties.PhysicalWidth,
				layer.SurfaceProperties.PhysicalHeight);
			result.EstimatedSceneCompositionSurfaceBytes = SaturatingAdd(
				result.EstimatedSceneCompositionSurfaceBytes, bytes);
			const auto* surfaceGraphics =
				static_cast<const CompositionSurfaceGraphics*>(
					layer.Graphics.get());
			if (surfaceGraphics->UsesDirect2DUpdateContext())
				++result.SceneLayerDirect2DSurfaceContextCount;
			const uint64_t snapshotBytes =
				surfaceGraphics->SubmittedTextureBytes();
			if (snapshotBytes > 0u)
				++result.SceneLayerSubmittedSnapshotTextureCount;
			result.EstimatedSceneSubmittedSnapshotBytes = SaturatingAdd(
				result.EstimatedSceneSubmittedSnapshotBytes, snapshotBytes);
			const auto snapshot = surfaceGraphics->SubmittedSnapshotStats();
			result.SceneLayerSubmittedSnapshotCreateCount +=
				snapshot.CreateCount;
			result.SceneLayerSubmittedSnapshotUpdateCount +=
				snapshot.UpdateCount;
			result.SceneLayerSubmittedSnapshotCopiedBytes +=
				snapshot.CopiedBytes;
			result.SceneLayerSubmittedSnapshotCreateMicroseconds +=
				snapshot.CreateMicroseconds;
			result.SceneLayerSubmittedSnapshotCopyMicroseconds +=
				snapshot.CopyMicroseconds;
		}
		else
		{
			++result.SceneLayerSwapChainCount;
			result.EstimatedSceneSwapChainBytes = SaturatingAdd(
				result.EstimatedSceneSwapChainBytes,
				EstimatedSwapChainBytes(
					layer.SurfaceProperties.PhysicalWidth,
					layer.SurfaceProperties.PhysicalHeight));
		}
	}
	result.SceneCommandRecorderCount = _sceneCommandRecorder ? 1u : 0u;
	result.SceneLayerDistinctGraphicsDeviceCount = sceneGraphicsDevices.size();
	result.SceneLayerDistinctRecorderDeviceCount = sceneRecorderDevices.size();
	result.EstimatedSceneLayerSlotBytes = SaturatingMultiply(
		static_cast<uint64_t>(_sceneLayers.capacity()),
		static_cast<uint64_t>(sizeof(SceneLayer)));
	result.EstimatedSceneRetainedBytes = SaturatingAdd(
		SaturatingAdd(
			SaturatingAdd(result.EstimatedSceneSwapChainBytes,
				result.EstimatedSceneCompositionSurfaceBytes),
			result.EstimatedSceneSubmittedSnapshotBytes),
		result.EstimatedSceneLayerSlotBytes);
	if (_primary)
	{
		result.EstimatedPrimarySwapChainBytes =
			EstimatedSwapChainBytes(_width, _height);
		result.PrimaryPresentSyncInterval =
			_primary->GetPresentSyncInterval();
	}
	if (_overlay)
	{
		result.EstimatedOverlaySwapChainBytes =
			EstimatedSwapChainBytes(_width, _height);
		result.OverlayPresentSyncInterval =
			_overlay->GetPresentSyncInterval();
	}
	result.EstimatedTotalSwapChainBytes = SaturatingAdd(
		SaturatingAdd(
			result.EstimatedPrimarySwapChainBytes,
			result.EstimatedOverlaySwapChainBytes),
		result.EstimatedSceneSwapChainBytes);
	result.PeakSceneLayerCount = _peakSceneLayerCount;
	result.PeakSceneLayerSlotCount = _peakSceneLayerSlotCount;
	result.PeakEstimatedSceneSwapChainBytes =
		_peakEstimatedSceneSwapChainBytes;
	result.PeakEstimatedSceneCompositionSurfaceBytes =
		_peakEstimatedSceneCompositionSurfaceBytes;
	result.PeakEstimatedSceneSubmittedSnapshotBytes =
		_peakEstimatedSceneSubmittedSnapshotBytes;
	result.PeakEstimatedSceneLayerSlotBytes =
		_peakEstimatedSceneLayerSlotBytes;
	result.SceneLayerCreateCount = _sceneLayerCreateCount;
	result.SceneLayerResizeCount = _sceneLayerResizeCount;
	result.SceneLayerReleaseCount = _sceneLayerReleaseCount;
	result.SceneLayerAllocationFailureCount =
		_sceneLayerAllocationFailureCount;
	result.SceneLayerSlotEnsureMicroseconds =
		_sceneLayerSlotEnsureMicroseconds;
	result.SceneLayerTopologyBatchBeginMicroseconds =
		_sceneLayerTopologyBatchBeginMicroseconds;
	result.SceneLayerSwapChainCreateMicroseconds =
		_sceneLayerSwapChainCreateMicroseconds;
	result.SceneLayerCompositionSurfaceCreateMicroseconds =
		_sceneLayerCompositionSurfaceCreateMicroseconds;
	result.SceneLayerVisualCreateMicroseconds =
		_sceneLayerVisualCreateMicroseconds;
	result.SceneLayerVisualBindMicroseconds =
		_sceneLayerVisualBindMicroseconds;
	result.SceneLayerGraphicsCreateMicroseconds =
		_sceneLayerGraphicsCreateMicroseconds;
	result.SceneLayerRecorderCreateMicroseconds =
		_sceneLayerRecorderCreateMicroseconds;
	result.SceneLayerDpiSetupMicroseconds =
		_sceneLayerDpiSetupMicroseconds;
	result.SceneLayerVisualPropertyStageMicroseconds =
		_sceneLayerVisualPropertyStageMicroseconds;
	result.SceneLayerGroupStageMicroseconds =
		_sceneLayerGroupStageMicroseconds;
	result.SceneLayerResourcePeakUpdateMicroseconds =
		_sceneLayerResourcePeakUpdateMicroseconds;
	result.SceneLayerTopologyBatchCommitMicroseconds =
		_sceneLayerTopologyBatchCommitMicroseconds;
	if (_composition)
	{
		const auto topology = _composition->GetVisualTopologyStatistics();
		result.CompositionVisualStackRebuildCount =
			topology.StackRebuildCount;
		result.CompositionVisualStackRebuildEntryCount =
			topology.StackRebuildEntryCount;
		result.CompositionVisualDeferredMutationCount =
			topology.DeferredMutationCount;
		result.CompositionVisualBatchCommitCount = topology.BatchCommitCount;
		result.CompositionVisualBatchRollbackCount =
			topology.BatchRollbackCount;
		result.CompositionVisualBatchRollbackFailureCount =
			topology.BatchRollbackFailureCount;
	}
	return result;
}

bool PresentationRenderHost::CreateCompositionResources()
{
#ifdef CUI_ENABLE_WEBVIEW2
	auto composition = std::make_unique<DCompLayeredHost>();
	if (!composition->Initialize(_window, _width, _height)) return false;
	auto* swapChain = static_cast<IDXGISwapChain1*>(
		composition->GetSwapChain());
	if (!swapChain) return false;

	auto primary = std::make_unique<CompositionSwapChainGraphics>(swapChain);
	if (!primary->GetDeviceContextRaw()) return false;
	std::unique_ptr<D2DGraphics> overlay;
	if (auto* overlaySwapChain = static_cast<IDXGISwapChain1*>(
		composition->GetOverlaySwapChain()))
	{
		overlay = std::make_unique<CompositionSwapChainGraphics>(
			overlaySwapChain);
		if (!overlay->GetDeviceContextRaw()) return false;
	}

	const auto dpi = static_cast<FLOAT>(_dpi);
	primary->SetDpi(dpi, dpi);
	if (overlay) overlay->SetDpi(dpi, dpi);
	_overlay = std::move(overlay);
	_primary = std::move(primary);
	_composition = std::move(composition);
	_active = nullptr;
	return true;
#else
	return false;
#endif
}

bool PresentationRenderHost::EnsureComposition()
{
#ifdef CUI_ENABLE_WEBVIEW2
	if (UsesComposition()) return true;
	if (!IsAttached() || _transactionOpen) return false;
	if (!CreateCompositionResources()) return false;
	AdvanceResourceGeneration();
	AdvanceDeviceResourceGeneration();
	InvalidateFrameHistory();
	return true;
#else
	return false;
#endif
}

bool PresentationRenderHost::UsesComposition() const noexcept
{
	return _composition && _composition->IsInitialized();
}

size_t PresentationRenderHost::SceneLayerCount() const noexcept
{
	return static_cast<size_t>(std::count_if(
		_sceneLayers.begin(), _sceneLayers.end(),
		[](const SceneLayer& layer)
		{ return layer.Visual && layer.Graphics; }));
}

bool PresentationRenderHost::EnsureSceneLayerSlots(size_t count) noexcept
{
	ScopedMicrosecondAccumulator timing(_sceneLayerSlotEnsureMicroseconds);
	if (_transactionOpen) return false;
	if (count <= _sceneLayers.size()) return true;
	try
	{
		_sceneLayers.resize(count);
		UpdateSceneResourcePeaks();
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool PresentationRenderHost::IsSceneLayerMaterialized(
	size_t index) const noexcept
{
	return index < _sceneLayers.size()
		&& _sceneLayers[index].Visual
		&& _sceneLayers[index].Graphics;
}

bool PresentationRenderHost::BeginSceneLayerTopologyBatch() noexcept
{
	ScopedMicrosecondAccumulator timing(
		_sceneLayerTopologyBatchBeginMicroseconds);
#ifdef CUI_ENABLE_WEBVIEW2
	if (_sceneLayerTopologyBatchActive || _transactionOpen || !_composition
		|| !_composition->BeginVisualTopologyBatch()) return false;
	_sceneLayerTopologyBatchInitialCount = _sceneLayers.size();
	_sceneLayerBatchResourceMutationPending = false;
	_sceneLayerTopologyBatchActive = true;
	return true;
#else
	return false;
#endif
}

bool PresentationRenderHost::CommitSceneLayerTopologyBatch() noexcept
{
	ScopedMicrosecondAccumulator timing(
		_sceneLayerTopologyBatchCommitMicroseconds);
#ifdef CUI_ENABLE_WEBVIEW2
	if (!_sceneLayerTopologyBatchActive || !_composition) return false;
	const size_t initialCount = _sceneLayerTopologyBatchInitialCount;
	const bool committed = _composition->CommitVisualTopologyBatch();
	if (!committed && !_composition->IsVisualTopologyBatchHealthy())
	{
		_deviceResetRequested = true;
		InvalidateFrameHistory();
	}
	if (committed) AcceptSceneLayerTopologyBatch();
	else DiscardSceneLayerTopologyBatch(initialCount);
	_sceneLayerTopologyBatchActive = false;
	_sceneLayerTopologyBatchInitialCount = 0;
	_sceneLayerBatchResourceMutationPending = false;
	return committed;
#else
	return false;
#endif
}

void PresentationRenderHost::RollbackSceneLayerTopologyBatch() noexcept
{
#ifdef CUI_ENABLE_WEBVIEW2
	if (!_sceneLayerTopologyBatchActive) return;
	const size_t initialCount = _sceneLayerTopologyBatchInitialCount;
	if (_composition) _composition->RollbackVisualTopologyBatch();
	DiscardSceneLayerTopologyBatch(initialCount);
	_sceneLayerTopologyBatchActive = false;
	_sceneLayerTopologyBatchInitialCount = 0;
#endif
}

void PresentationRenderHost::
FailNextSceneLayerTopologyBatchCommitForTesting() noexcept
{
	DCompLayeredHost::FailNextVisualTopologyBatchCommitForTesting();
}

void PresentationRenderHost::
ClearSceneLayerTopologyBatchCommitFailureForTesting() noexcept
{
	DCompLayeredHost::ClearVisualTopologyBatchCommitFailureForTesting();
}

void PresentationRenderHost::
FailNextSceneLayerGroupTopologyStageForTesting() noexcept
{
	_failNextSceneLayerGroupTopologyStageForTesting.store(
		true, std::memory_order_release);
}

void PresentationRenderHost::
ClearSceneLayerGroupTopologyStageFailureForTesting() noexcept
{
	_failNextSceneLayerGroupTopologyStageForTesting.store(
		false, std::memory_order_release);
}

bool PresentationRenderHost::ExchangeSceneCompositionSurfaceBackendForTesting(
	bool enabled) noexcept
{
	return _useSceneCompositionSurfaces.exchange(
		enabled, std::memory_order_acq_rel);
}

bool PresentationRenderHost::
AcquireSceneLayerPixelReadbackLeaseForTesting() noexcept
{
	if (_transactionOpen
		|| _sceneLayerPixelReadbackLeaseCount
			== (std::numeric_limits<size_t>::max)()) return false;
	if (_sceneLayerPixelReadbackLeaseCount == 0u)
	{
		for (const auto& layer : _sceneLayers)
		{
			if (!layer.UsesCompositionSurface || !layer.Graphics) continue;
			const auto* graphics =
				static_cast<const CompositionSurfaceGraphics*>(
					layer.Graphics.get());
			if (!graphics->CanEnableSubmittedTextureCapture()) return false;
		}
		for (auto& layer : _sceneLayers)
		{
			if (!layer.UsesCompositionSurface || !layer.Graphics) continue;
			static_cast<CompositionSurfaceGraphics*>(layer.Graphics.get())->
				SetSubmittedTextureCaptureEnabled(true);
		}
	}
	++_sceneLayerPixelReadbackLeaseCount;
	return true;
}

void PresentationRenderHost::
ReleaseSceneLayerPixelReadbackLeaseForTesting() noexcept
{
	if (_sceneLayerPixelReadbackLeaseCount == 0u) return;
	if (--_sceneLayerPixelReadbackLeaseCount != 0u) return;
	UpdateSceneResourcePeaks();
	for (auto& layer : _sceneLayers)
	{
		if (!layer.UsesCompositionSurface || !layer.Graphics) continue;
		static_cast<CompositionSurfaceGraphics*>(layer.Graphics.get())->
			SetSubmittedTextureCaptureEnabled(false);
	}
}

D2DGraphics* PresentationRenderHost::AcquireSceneLayer(
	size_t index,
	int layer,
	int order)
{
	return AcquireSceneLayer(index, layer, order,
		SceneLayerSurfaceProperties{
			(std::max)(UINT{ 1 }, _width),
			(std::max)(UINT{ 1 }, _height), true });
}

D2DGraphics* PresentationRenderHost::AcquireSceneLayer(
	size_t index,
	int layer,
	int order,
	const SceneLayerSurfaceProperties& requestedSurfaceProperties)
{
#ifdef CUI_ENABLE_WEBVIEW2
	if (!UsesComposition() || _transactionOpen) return nullptr;
	SceneLayerSurfaceProperties surfaceProperties = requestedSurfaceProperties;
	surfaceProperties.PhysicalWidth = (std::max)(
		UINT{ 1 }, surfaceProperties.PhysicalWidth);
	surfaceProperties.PhysicalHeight = (std::max)(
		UINT{ 1 }, surfaceProperties.PhysicalHeight);
	if (!EnsureSceneLayerSlots(index + 1u)) return nullptr;
	auto& item = _sceneLayers[index];
	if (!item.Visual && !item.Graphics
		&& (item.ContentVisual || item.Recorder || item.EffectGroup
			|| item.Clip || !item.IntermediateClipVisuals.empty()
			|| !item.IntermediateClips.empty()))
	{
		_deviceResetRequested = true;
		InvalidateFrameHistory();
		return nullptr;
	}
	if (!item.Visual && !item.Graphics)
	{
		uint64_t remaining =
			_failSceneLayerAllocationAfterForTesting.load(
				std::memory_order_acquire);
		while (remaining != UINT64_MAX)
		{
			if (remaining == 0)
			{
				++_sceneLayerAllocationFailureCount;
				return nullptr;
			}
			if (_failSceneLayerAllocationAfterForTesting.
				compare_exchange_weak(
					remaining, remaining - 1u,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) break;
		}
		void* swapChainPointer = nullptr;
		IDCompositionSurface* compositionSurface = nullptr;
		IDCompositionVisual* visual = nullptr;
		IDCompositionVisual* contentVisual = nullptr;
		DCompLayeredHost::D2DLayerCreationTimings creationTimings;
		const bool useCompositionSurface =
			_useSceneCompositionSurfaces.load(std::memory_order_acquire);
		const bool layerCreated = useCompositionSurface
			? _composition->CreateD2DSurfaceLayer(
				&compositionSurface, &visual, &contentVisual,
				surfaceProperties.PhysicalWidth,
				surfaceProperties.PhysicalHeight,
				layer, order, &creationTimings)
			: _composition->CreateD2DLayer(
				&swapChainPointer, &visual, &contentVisual,
				surfaceProperties.PhysicalWidth,
				surfaceProperties.PhysicalHeight,
				layer, order, &creationTimings);
		_sceneLayerSwapChainCreateMicroseconds +=
			creationTimings.SwapChainMicroseconds;
		_sceneLayerCompositionSurfaceCreateMicroseconds +=
			creationTimings.SurfaceMicroseconds;
		_sceneLayerVisualCreateMicroseconds +=
			creationTimings.VisualCreationMicroseconds;
		_sceneLayerVisualBindMicroseconds +=
			creationTimings.VisualBindingMicroseconds;
		if (!layerCreated)
		{
			++_sceneLayerAllocationFailureCount;
			return nullptr;
		}
		auto* swapChain = static_cast<IDXGISwapChain1*>(swapChainPointer);
		std::unique_ptr<D2DGraphics> graphics;
		{
			ScopedMicrosecondAccumulator timing(
				_sceneLayerGraphicsCreateMicroseconds);
			if (useCompositionSurface)
			{
				auto surfaceGraphics =
					std::make_unique<CompositionSurfaceGraphics>(
						compositionSurface, _composition->GetD2DDevice(),
						surfaceProperties.PhysicalWidth,
						surfaceProperties.PhysicalHeight,
						_composition->SupportsD2DSurfaceDeviceContexts());
				surfaceGraphics->SetSubmittedTextureCaptureEnabled(
					_sceneLayerPixelReadbackLeaseCount > 0u);
				graphics = std::move(surfaceGraphics);
			}
			else
				graphics = std::make_unique<CompositionSwapChainGraphics>(
					swapChain, 0u, _composition->GetD2DDevice());
		}
		if (swapChain) swapChain->Release();
		if (compositionSurface) compositionSurface->Release();
		if (!graphics->GetDeviceContextRaw())
		{
			if (visual)
			{
				_composition->DestroyD2DLayer(visual);
				visual->Release();
			}
			if (contentVisual) contentVisual->Release();
			++_sceneLayerAllocationFailureCount;
			return nullptr;
		}
		const bool createSharedRecorder = !_sceneCommandRecorder;
		if (createSharedRecorder)
		{
			ScopedMicrosecondAccumulator timing(
				_sceneLayerRecorderCreateMicroseconds);
			_sceneCommandRecorder = std::make_unique<D2DGraphics>(
				graphics->GetDeviceRaw());
		}
		if (!_sceneCommandRecorder
			|| !_sceneCommandRecorder->GetDeviceContextRaw())
		{
			_sceneCommandRecorder.reset();
			if (visual)
			{
				_composition->DestroyD2DLayer(visual);
				visual->Release();
			}
			if (contentVisual) contentVisual->Release();
			++_sceneLayerAllocationFailureCount;
			return nullptr;
		}
		{
			ScopedMicrosecondAccumulator timing(
				_sceneLayerDpiSetupMicroseconds);
			const auto dpi = static_cast<FLOAT>(_dpi);
			graphics->SetDpi(dpi, dpi);
			if (createSharedRecorder)
				_sceneCommandRecorder->SetDpi(dpi, dpi);
		}
		item.Visual = visual;
		item.ContentVisual = contentVisual;
		item.Graphics = std::move(graphics);
		item.Recorder = _sceneCommandRecorder.get();
		item.SurfaceProperties = surfaceProperties;
		item.Layer = layer;
		item.Order = order;
		item.GroupIndex = SceneLayerGroupProperties::NoParent;
		item.CreatedInTopologyBatch = _sceneLayerTopologyBatchActive;
		item.UsesCompositionSurface = useCompositionSurface;
		++_sceneLayerCreateCount;
		if (_sceneLayerTopologyBatchActive)
			_sceneLayerBatchResourceMutationPending = true;
		else
		{
			UpdateSceneResourcePeaks();
			AdvanceResourceGeneration();
			InvalidateFrameHistory();
		}
	}
	else if (!item.Visual || !item.ContentVisual
		|| !item.Graphics || !item.Recorder)
	{
		_deviceResetRequested = true;
		InvalidateFrameHistory();
		return nullptr;
	}
	const bool orderChanged = item.Layer != layer || item.Order != order;
	item.Layer = layer;
	item.Order = order;
	if (!(item.SurfaceProperties == surfaceProperties))
	{
		if (!item.Graphics) return nullptr;
		item.Graphics->ReSize(
			surfaceProperties.PhysicalWidth,
			surfaceProperties.PhysicalHeight);
		if (item.Graphics->IsDeviceLost())
		{
			_deviceResetRequested = true;
			InvalidateFrameHistory();
			return nullptr;
		}
		item.SurfaceProperties = surfaceProperties;
		++_sceneLayerResizeCount;
		UpdateSceneResourcePeaks();
		AdvanceResourceGeneration();
		InvalidateFrameHistory();
	}
	if (orderChanged && item.Visual
		&& item.GroupIndex == static_cast<size_t>(-1))
		_composition->UpdateVisualOrder(item.Visual, layer, order);
	return item.Graphics.get();
#else
	(void)index;
	(void)layer;
	(void)order;
	(void)requestedSurfaceProperties;
	return nullptr;
#endif
}

bool PresentationRenderHost::StageSceneLayerGroups(
	std::span<const SceneLayerGroupProperties> groups) noexcept
{
	ScopedMicrosecondAccumulator timing(_sceneLayerGroupStageMicroseconds);
#ifdef CUI_ENABLE_WEBVIEW2
	if (_transactionOpen || !UsesComposition()) return false;
	auto fail = [this]() noexcept
	{
		_deviceResetRequested = true;
		InvalidateFrameHistory();
		return false;
	};
	try
	{
		std::unordered_set<IDCompositionVisual*> nativeVisuals;
		for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex)
		{
			const auto& group = groups[groupIndex];
			if (group.LayerCount == 0u
				|| group.FirstLayer > _sceneLayers.size()
				|| group.LayerCount > _sceneLayers.size() - group.FirstLayer
				|| !IsFinite(group))
				return false;
			const size_t groupEnd = group.FirstLayer + group.LayerCount;
			if (group.ParentGroup != SceneLayerGroupProperties::NoParent)
			{
				if (group.ParentGroup >= groupIndex) return false;
				const auto& parent = groups[group.ParentGroup];
				const size_t parentEnd =
					parent.FirstLayer + parent.LayerCount;
				if (group.FirstLayer < parent.FirstLayer
					|| groupEnd > parentEnd
					|| (group.FirstLayer == parent.FirstLayer
						&& groupEnd == parentEnd)) return false;
			}
			for (size_t previous = 0; previous < groupIndex; ++previous)
			{
				const auto& sibling = groups[previous];
				if (sibling.ParentGroup != group.ParentGroup) continue;
				const size_t siblingEnd =
					sibling.FirstLayer + sibling.LayerCount;
				if (group.FirstLayer < siblingEnd
					&& sibling.FirstLayer < groupEnd) return false;
			}
			for (const auto& native : group.NativeVisuals)
			{
				if (!native.Visual || !nativeVisuals.insert(native.Visual).second)
					return false;
				if (std::any_of(_sceneLayers.begin(), _sceneLayers.end(),
					[&native](const SceneLayer& layer)
					{ return layer.Visual == native.Visual; })) return false;
			}
		}
		auto* device = _composition ? _composition->GetDCompDevice() : nullptr;
		if (!device && !groups.empty()) return fail();
		auto stageVisualProperties = [](
			SceneLayerGroup& accepted,
			const SceneLayerGroupProperties& properties)
		{
			if (!accepted.Visual || !accepted.ContentVisual
				|| !accepted.EffectGroup) return false;
			D2D1_MATRIX_3X2_F rootTransform{};
			D2D1_MATRIX_3X2_F contentTransform{};
			std::vector<D2D1_MATRIX_3X2_F> intermediateTransforms;
			const auto identity = D2D1::Matrix3x2F::Identity();
			if (!ResolveClipTransforms(properties.TransformedClipChain,
				identity, rootTransform, intermediateTransforms,
				contentTransform)) return false;
			const bool hasRootClip = properties.HasClip
				|| !properties.TransformedClipChain.empty();
			const size_t requiredIntermediate =
				properties.TransformedClipChain.empty() ? 0u
				: properties.TransformedClipChain.size() - 1u;
			if ((hasRootClip && !accepted.Clip)
				|| (!hasRootClip && accepted.Clip)
				|| requiredIntermediate
					!= accepted.IntermediateClipVisuals.size()
				|| requiredIntermediate
					!= accepted.IntermediateClips.size()) return false;

			HRESULT result = S_OK;
			if (properties.HasClip)
				result = SetRectangleClip(
					accepted.Clip, properties.PhysicalClip);
			else if (!properties.TransformedClipChain.empty())
			{
				const auto& clip = properties.TransformedClipChain.front();
				result = SetRectangleClip(accepted.Clip,
					clip.PhysicalClip, clip.RadiusX, clip.RadiusY);
			}
			for (size_t index = 0;
				SUCCEEDED(result) && index < requiredIntermediate; ++index)
			{
				const auto& clip =
					properties.TransformedClipChain[index + 1u];
				result = SetRectangleClip(accepted.IntermediateClips[index],
					clip.PhysicalClip, clip.RadiusX, clip.RadiusY);
				if (SUCCEEDED(result))
					result = accepted.IntermediateClipVisuals[index]->
						SetTransform(intermediateTransforms[index]);
			}
			if (SUCCEEDED(result))
				result = accepted.Visual->SetTransform(rootTransform);
			if (SUCCEEDED(result))
				result = accepted.ContentVisual->SetTransform(contentTransform);
			if (SUCCEEDED(result))
				result = accepted.EffectGroup->SetOpacity(properties.Opacity);
			return SUCCEEDED(result);
		};

		bool topologyChanged = groups.size() != _sceneLayerGroups.size();
		for (size_t index = 0; !topologyChanged && index < groups.size(); ++index)
			topologyChanged = !_sceneLayerGroups[index].Properties.
				HasSameTopology(groups[index]);
		for (size_t layerIndex = 0;
			!topologyChanged && layerIndex < _sceneLayers.size(); ++layerIndex)
		{
			const auto& layer = _sceneLayers[layerIndex];
			if (!layer.Visual) continue;
			size_t expectedOwner = SceneLayerGroupProperties::NoParent;
			for (size_t groupIndex = 0;
				groupIndex < groups.size(); ++groupIndex)
			{
				const auto& group = groups[groupIndex];
				if (layerIndex >= group.FirstLayer
					&& layerIndex < group.FirstLayer + group.LayerCount)
					expectedOwner = groupIndex;
			}
			topologyChanged = layer.GroupIndex != expectedOwner;
		}
		// The midpoint failure seam must be deterministic even when the accepted
		// and requested graphs have identical topology. Force a test-only rebuild
		// so the injected edge always executes after detaching accepted parents.
		if (_failNextSceneLayerGroupTopologyStageForTesting.load(
			std::memory_order_acquire)) topologyChanged = true;
		if (topologyChanged)
		{
			ReleaseSceneLayerGroups(true);
			if (_deviceResetRequested) return false;
			// This seam sits at the dangerous midpoint: the previously accepted
			// parents are detached and newly materialized layers may already have
			// entered the visual registry, but no replacement group exists. A real
			// failure here must force device recovery so an unrelated composition
			// commit can never publish that partial graph.
			if (_failNextSceneLayerGroupTopologyStageForTesting.exchange(
				false, std::memory_order_acq_rel)) return fail();
			_sceneLayerGroups.reserve(groups.size());
			for (const auto& properties : groups)
			{
				ComPtr<IDCompositionVisual> visual;
				ComPtr<IDCompositionVisual> contentVisual;
				ComPtr<IDCompositionEffectGroup> effect;
				ComPtr<IDCompositionRectangleClip> clip;
				const bool hasRootClip = properties.HasClip
					|| !properties.TransformedClipChain.empty();
				const size_t requiredIntermediate =
					properties.TransformedClipChain.empty() ? 0u
					: properties.TransformedClipChain.size() - 1u;
				std::vector<ComPtr<IDCompositionVisual>> intermediateVisuals(
					requiredIntermediate);
				std::vector<ComPtr<IDCompositionRectangleClip>> intermediateClips(
					requiredIntermediate);
				if (FAILED(device->CreateVisual(
					visual.ReleaseAndGetAddressOf())) || !visual
					|| FAILED(device->CreateVisual(
						contentVisual.ReleaseAndGetAddressOf())) || !contentVisual
					|| FAILED(device->CreateEffectGroup(
						effect.ReleaseAndGetAddressOf())) || !effect
					|| FAILED(contentVisual->SetEffect(effect.Get())))
				{
					ReleaseSceneLayerGroups(true);
					return fail();
				}
				if (hasRootClip
					&& (FAILED(device->CreateRectangleClip(
						clip.ReleaseAndGetAddressOf())) || !clip
						|| FAILED(visual->SetClip(clip.Get()))))
				{
					ReleaseSceneLayerGroups(true);
					return fail();
				}
				for (size_t clipIndex = 0;
					clipIndex < requiredIntermediate; ++clipIndex)
				{
					if (FAILED(device->CreateVisual(
						intermediateVisuals[clipIndex].ReleaseAndGetAddressOf()))
						|| !intermediateVisuals[clipIndex]
						|| FAILED(device->CreateRectangleClip(
							intermediateClips[clipIndex].ReleaseAndGetAddressOf()))
						|| !intermediateClips[clipIndex]
						|| FAILED(intermediateVisuals[clipIndex]->SetClip(
							intermediateClips[clipIndex].Get())))
					{
						ReleaseSceneLayerGroups(true);
						return fail();
					}
					if (clipIndex > 0u
						&& FAILED(intermediateVisuals[clipIndex - 1u]->AddVisual(
							intermediateVisuals[clipIndex].Get(), FALSE, nullptr)))
					{
						ReleaseSceneLayerGroups(true);
						return fail();
					}
				}
				auto* contentParent = requiredIntermediate == 0u
					? visual.Get() : intermediateVisuals.back().Get();
				if (FAILED(contentParent->AddVisual(
					contentVisual.Get(), FALSE, nullptr))
					|| (requiredIntermediate > 0u
						&& FAILED(visual->AddVisual(
							intermediateVisuals.front().Get(), FALSE, nullptr))))
				{
					ReleaseSceneLayerGroups(true);
					return fail();
				}
				_sceneLayerGroups.push_back(SceneLayerGroup{
					visual.Detach(), contentVisual.Detach(), effect.Detach(),
					clip.Detach(), {}, {}, properties });
				auto& accepted = _sceneLayerGroups.back();
				accepted.IntermediateClipVisuals.reserve(requiredIntermediate);
				accepted.IntermediateClips.reserve(requiredIntermediate);
				for (size_t clipIndex = 0;
					clipIndex < requiredIntermediate; ++clipIndex)
				{
					accepted.IntermediateClipVisuals.push_back(
						intermediateVisuals[clipIndex].Detach());
					accepted.IntermediateClips.push_back(
						intermediateClips[clipIndex].Detach());
				}
				if (!stageVisualProperties(accepted, properties))
				{
					ReleaseSceneLayerGroups(true);
					return fail();
				}
			}

			struct OrderedChild final
			{
				IDCompositionVisual* Visual = nullptr;
				int Order = 0;
				size_t Sequence = 0;
			};
			std::vector<std::vector<OrderedChild>> children(groups.size());
			size_t childSequence = 0;
			for (size_t groupIndex = 0;
				groupIndex < groups.size(); ++groupIndex)
			{
				const auto& properties = groups[groupIndex];
				auto& group = _sceneLayerGroups[groupIndex];
				if (properties.ParentGroup
					== SceneLayerGroupProperties::NoParent)
				{
					if (!_composition->RegisterVisual(
						group.Visual, properties.Layer, properties.Order))
					{
						ReleaseSceneLayerGroups(true);
						return fail();
					}
				}
				else
				{
					children[properties.ParentGroup].push_back({
						group.Visual, properties.Order, childSequence++ });
				}
			}
			for (size_t layerIndex = 0;
				layerIndex < _sceneLayers.size(); ++layerIndex)
			{
				size_t owner = SceneLayerGroupProperties::NoParent;
				for (size_t groupIndex = 0;
					groupIndex < groups.size(); ++groupIndex)
				{
					const auto& group = groups[groupIndex];
					if (layerIndex >= group.FirstLayer
						&& layerIndex < group.FirstLayer + group.LayerCount)
						owner = groupIndex;
				}
				if (owner == SceneLayerGroupProperties::NoParent) continue;
				auto& layer = _sceneLayers[layerIndex];
				if (!layer.Visual) continue;
				if (!layer.Graphics
					|| layer.GroupIndex != SceneLayerGroupProperties::NoParent)
				{
					ReleaseSceneLayerGroups(true);
					return fail();
				}
				_composition->UnregisterVisual(layer.Visual);
				layer.GroupIndex = owner;
				children[owner].push_back({
					layer.Visual, layer.Order, childSequence++ });
			}
			for (size_t groupIndex = 0;
				groupIndex < groups.size(); ++groupIndex)
			{
				for (const auto& native : groups[groupIndex].NativeVisuals)
				{
					_composition->UnregisterVisual(native.Visual);
					children[groupIndex].push_back({
						native.Visual, native.Order, childSequence++ });
				}
			}
			for (size_t groupIndex = 0;
				groupIndex < children.size(); ++groupIndex)
			{
				auto& ordered = children[groupIndex];
				std::stable_sort(ordered.begin(), ordered.end(),
					[](const OrderedChild& left, const OrderedChild& right)
					{
						if (left.Order != right.Order)
							return left.Order < right.Order;
						return left.Sequence < right.Sequence;
					});
				auto* parent = _sceneLayerGroups[groupIndex].ContentVisual;
				if (!parent)
				{
					ReleaseSceneLayerGroups(true);
					return fail();
				}
				for (const auto& child : ordered)
				{
					if (!child.Visual || FAILED(parent->AddVisual(
						child.Visual, FALSE, nullptr)))
					{
						ReleaseSceneLayerGroups(true);
						return fail();
					}
				}
			}
			AdvanceResourceGeneration();
			InvalidateFrameHistory();
		}

		for (size_t index = 0; index < groups.size(); ++index)
		{
			auto& accepted = _sceneLayerGroups[index];
			if (!stageVisualProperties(accepted, groups[index]))
				return fail();
			accepted.Properties = groups[index];
		}
		return true;
	}
	catch (...)
	{
		return fail();
	}
#else
	(void)groups;
	return false;
#endif
}

bool PresentationRenderHost::StageSceneLayerVisualProperties(
	size_t index,
	const SceneLayerVisualProperties& properties) noexcept
{
	ScopedMicrosecondAccumulator timing(
		_sceneLayerVisualPropertyStageMicroseconds);
#ifdef CUI_ENABLE_WEBVIEW2
	if (_transactionOpen || index >= _sceneLayers.size()
		|| !IsFinite(properties)) return false;
	auto& item = _sceneLayers[index];
	if (!item.Visual || !item.ContentVisual) return false;
	if (item.VisualProperties == properties) return true;
	auto fail = [this]() noexcept
	{
		_deviceResetRequested = true;
		InvalidateFrameHistory();
		return false;
	};
	try
	{
		SceneLayerVisualProperties accepted = properties;
		auto* device = _composition ? _composition->GetDCompDevice() : nullptr;
		if (!device) return fail();

		D2D1_MATRIX_3X2_F rootTransform{};
		D2D1_MATRIX_3X2_F contentTransform{};
		std::vector<D2D1_MATRIX_3X2_F> intermediateTransforms;
		if (!ResolveClipTransforms(
			properties.TransformedClipChain, properties.PhysicalTransform,
			rootTransform,
			intermediateTransforms, contentTransform)) return false;
		D2D1_MATRIX_3X2_F previousRootTransform{};
		D2D1_MATRIX_3X2_F previousContentTransform{};
		std::vector<D2D1_MATRIX_3X2_F> previousIntermediateTransforms;
		if (!ResolveClipTransforms(
			item.VisualProperties.TransformedClipChain,
			item.VisualProperties.PhysicalTransform,
			previousRootTransform,
			previousIntermediateTransforms,
			previousContentTransform)) return fail();

		ComPtr<IDCompositionEffectGroup> newEffectGroup;
		auto* effectGroup = item.EffectGroup;
		if (!effectGroup && properties.Opacity != 1.0f)
		{
			if (FAILED(device->CreateEffectGroup(
				newEffectGroup.ReleaseAndGetAddressOf())) || !newEffectGroup)
				return fail();
			effectGroup = newEffectGroup.Get();
		}

		const bool hasRootClip = properties.HasClip
			|| !properties.TransformedClipChain.empty();
		ComPtr<IDCompositionRectangleClip> newRootClip;
		auto* rootClip = item.Clip;
		if (!rootClip && hasRootClip)
		{
			if (FAILED(device->CreateRectangleClip(
				newRootClip.ReleaseAndGetAddressOf())) || !newRootClip)
				return fail();
			rootClip = newRootClip.Get();
		}

		const size_t requiredIntermediate =
			properties.TransformedClipChain.empty() ? 0u
			: properties.TransformedClipChain.size() - 1u;
		const bool topologyChanged = requiredIntermediate
			!= item.IntermediateClipVisuals.size();
		std::vector<ComPtr<IDCompositionVisual>> newVisuals;
		std::vector<ComPtr<IDCompositionRectangleClip>> newClips;
		if (topologyChanged)
		{
			newVisuals.resize(requiredIntermediate);
			newClips.resize(requiredIntermediate);
			for (size_t clipIndex = 0; clipIndex < requiredIntermediate;
				++clipIndex)
			{
				if (FAILED(device->CreateVisual(
					newVisuals[clipIndex].ReleaseAndGetAddressOf()))
					|| !newVisuals[clipIndex]
					|| FAILED(device->CreateRectangleClip(
						newClips[clipIndex].ReleaseAndGetAddressOf()))
					|| !newClips[clipIndex]
					|| FAILED(newVisuals[clipIndex]->SetClip(
						newClips[clipIndex].Get()))) return fail();
				if (clipIndex > 0u
					&& FAILED(newVisuals[clipIndex - 1u]->AddVisual(
						newVisuals[clipIndex].Get(), FALSE, nullptr)))
					return fail();
			}
		}

		HRESULT result = S_OK;
		if (hasRootClip)
		{
			if (properties.HasClip)
				result = SetRectangleClip(rootClip, properties.PhysicalClip);
			else
			{
				const auto& rootClipProperties =
					properties.TransformedClipChain.front();
				result = SetRectangleClip(rootClip,
					rootClipProperties.PhysicalClip,
					rootClipProperties.RadiusX,
					rootClipProperties.RadiusY);
			}
		}
		for (size_t clipIndex = 0;
			SUCCEEDED(result) && clipIndex < requiredIntermediate;
			++clipIndex)
		{
			auto* visual = topologyChanged
				? newVisuals[clipIndex].Get()
				: item.IntermediateClipVisuals[clipIndex];
			auto* clip = topologyChanged
				? newClips[clipIndex].Get()
				: item.IntermediateClips[clipIndex];
			const auto& clipProperties =
				properties.TransformedClipChain[clipIndex + 1u];
			result = SetRectangleClip(clip, clipProperties.PhysicalClip,
				clipProperties.RadiusX, clipProperties.RadiusY);
			if (SUCCEEDED(result))
				result = visual->SetTransform(intermediateTransforms[clipIndex]);
		}
		if (SUCCEEDED(result) && effectGroup
			&& (newEffectGroup
				|| item.VisualProperties.Opacity != properties.Opacity))
			result = effectGroup->SetOpacity(properties.Opacity);
		if (FAILED(result)) return fail();

		if (topologyChanged)
		{
			auto* oldContentParent = item.IntermediateClipVisuals.empty()
				? item.Visual : item.IntermediateClipVisuals.back();
			result = oldContentParent->RemoveVisual(item.ContentVisual);
			if (SUCCEEDED(result) && !item.IntermediateClipVisuals.empty())
				result = item.Visual->RemoveVisual(
					item.IntermediateClipVisuals.front());
			if (SUCCEEDED(result) && requiredIntermediate > 0u)
				result = newVisuals.back()->AddVisual(
					item.ContentVisual, FALSE, nullptr);
			if (SUCCEEDED(result) && requiredIntermediate > 0u)
				result = item.Visual->AddVisual(
					newVisuals.front().Get(), FALSE, nullptr);
			if (SUCCEEDED(result) && requiredIntermediate == 0u)
				result = item.Visual->AddVisual(
					item.ContentVisual, FALSE, nullptr);
			if (FAILED(result)) return fail();
		}

		if (!MatricesEqual(previousRootTransform, rootTransform))
			result = item.Visual->SetTransform(rootTransform);
		if (SUCCEEDED(result)
			&& !MatricesEqual(previousContentTransform, contentTransform))
			result = item.ContentVisual->SetTransform(contentTransform);
		const bool previouslyHadRootClip =
			item.VisualProperties.HasClip
			|| !item.VisualProperties.TransformedClipChain.empty();
		if (SUCCEEDED(result) && previouslyHadRootClip != hasRootClip)
			result = item.Visual->SetClip(hasRootClip ? rootClip : nullptr);
		if (SUCCEEDED(result) && newEffectGroup)
			result = item.ContentVisual->SetEffect(newEffectGroup.Get());
		if (FAILED(result)) return fail();

		if (topologyChanged)
		{
			for (size_t clipIndex = 0;
				clipIndex < item.IntermediateClipVisuals.size(); ++clipIndex)
			{
				auto* visual = item.IntermediateClipVisuals[clipIndex];
				if (visual) (void)visual->SetClip(nullptr);
				if (clipIndex + 1u < item.IntermediateClipVisuals.size()
					&& visual) (void)visual->RemoveVisual(
						item.IntermediateClipVisuals[clipIndex + 1u]);
				if (item.IntermediateClips[clipIndex])
					item.IntermediateClips[clipIndex]->Release();
				if (visual) visual->Release();
			}
			item.IntermediateClipVisuals.clear();
			item.IntermediateClips.clear();
			item.IntermediateClipVisuals.reserve(requiredIntermediate);
			item.IntermediateClips.reserve(requiredIntermediate);
			for (size_t clipIndex = 0; clipIndex < requiredIntermediate;
				++clipIndex)
			{
				item.IntermediateClipVisuals.push_back(
					newVisuals[clipIndex].Detach());
				item.IntermediateClips.push_back(newClips[clipIndex].Detach());
			}
		}
		if (newEffectGroup)
			item.EffectGroup = newEffectGroup.Detach();
		if (newRootClip) item.Clip = newRootClip.Detach();
		item.VisualProperties = std::move(accepted);
		return true;
	}
	catch (...)
	{
		return fail();
	}
#else
	(void)index;
	(void)properties;
	return false;
#endif
}

bool PresentationRenderHost::TryGetSceneLayerVisualProperties(
	size_t index,
	SceneLayerVisualProperties& properties) const noexcept
{
	if (index >= _sceneLayers.size() || !_sceneLayers[index].Visual)
		return false;
	properties = _sceneLayers[index].VisualProperties;
	return true;
}

bool PresentationRenderHost::TryGetSceneLayerSurfaceProperties(
	size_t index,
	SceneLayerSurfaceProperties& properties) const noexcept
{
	if (index >= _sceneLayers.size() || !_sceneLayers[index].Visual)
		return false;
	properties = _sceneLayers[index].SurfaceProperties;
	return true;
}

bool PresentationRenderHost::TryGetSceneLayerPixelDigestForTesting(
	size_t index,
	UINT& width,
	UINT& height,
	uint64_t& digest,
	size_t& nonTransparentPixels) const noexcept
{
	width = 0;
	height = 0;
	digest = 0;
	nonTransparentPixels = 0;
	if (_transactionOpen || index >= _sceneLayers.size()) return false;
	const auto& layer = _sceneLayers[index];
	ComPtr<ID3D11Texture2D> source;
	if (layer.Graphics)
		source = layer.Graphics->GetSubmittedTextureForReadback();
	if (!source)
	{
		auto* swapChain = layer.Graphics
			? layer.Graphics->GetSwapChainRaw() : nullptr;
		if (!swapChain || layer.Graphics->RequiresFullPresentFrame()) return false;
		ComPtr<IDXGISwapChain3> swapChain3;
		DXGI_SWAP_CHAIN_DESC description{};
		if (FAILED(swapChain->QueryInterface(
			IID_PPV_ARGS(swapChain3.ReleaseAndGetAddressOf())))
			|| !swapChain3 || FAILED(swapChain->GetDesc(&description))
			|| description.BufferCount == 0) return false;
		const UINT current = swapChain3->GetCurrentBackBufferIndex();
		const UINT presented = (current + description.BufferCount - 1u)
			% description.BufferCount;
		if (FAILED(swapChain->GetBuffer(
			presented, IID_PPV_ARGS(source.ReleaseAndGetAddressOf())))
			|| !source) return false;
	}

	D3D11_TEXTURE2D_DESC textureDescription{};
	source->GetDesc(&textureDescription);
	if (textureDescription.Width == 0 || textureDescription.Height == 0
		|| (textureDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM
			&& textureDescription.Format
				!= DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) return false;
	textureDescription.Usage = D3D11_USAGE_STAGING;
	textureDescription.BindFlags = 0;
	textureDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	textureDescription.MiscFlags = 0;
	ComPtr<ID3D11Device> device;
	source->GetDevice(device.ReleaseAndGetAddressOf());
	if (!device) return false;
	ComPtr<ID3D11DeviceContext> context;
	device->GetImmediateContext(context.ReleaseAndGetAddressOf());
	if (!context) return false;
	ComPtr<ID3D11Texture2D> staging;
	if (FAILED(device->CreateTexture2D(
		&textureDescription, nullptr, staging.ReleaseAndGetAddressOf()))
		|| !staging) return false;
	context->CopyResource(staging.Get(), source.Get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(
		staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))
		|| !mapped.pData) return false;
	struct UnmapScope final
	{
		ID3D11DeviceContext* Context = nullptr;
		ID3D11Texture2D* Texture = nullptr;
		~UnmapScope()
		{
			if (Context && Texture) Context->Unmap(Texture, 0);
		}
	} unmap{ context.Get(), staging.Get() };

	constexpr uint64_t FnvOffset = 14695981039346656037ull;
	constexpr uint64_t FnvPrime = 1099511628211ull;
	uint64_t value = FnvOffset;
	size_t opaque = 0;
	const size_t rowBytes = static_cast<size_t>(textureDescription.Width) * 4u;
	for (UINT row = 0; row < textureDescription.Height; ++row)
	{
		const auto* bytes = static_cast<const uint8_t*>(mapped.pData)
			+ static_cast<size_t>(row) * mapped.RowPitch;
		for (size_t byte = 0; byte < rowBytes; ++byte)
		{
			value ^= bytes[byte];
			value *= FnvPrime;
		}
		for (size_t pixel = 0; pixel < rowBytes; pixel += 4u)
			if (bytes[pixel + 3u] != 0) ++opaque;
	}
	width = textureDescription.Width;
	height = textureDescription.Height;
	digest = value;
	nonTransparentPixels = opaque;
	return true;
}

void PresentationRenderHost::TrimSceneLayers(size_t usedCount) noexcept
{
	if (usedCount >= _sceneLayers.size() || _transactionOpen) return;
	if (!_sceneLayerGroups.empty()) ReleaseSceneLayerGroups(true);
	while (_sceneLayers.size() > usedCount)
	{
		ReleaseSceneLayerResources(_sceneLayers.back());
		_sceneLayers.pop_back();
	}
	if (SceneLayerCount() == 0u) _sceneCommandRecorder.reset();
	AdvanceResourceGeneration();
	InvalidateFrameHistory();
}

void PresentationRenderHost::ReleaseSceneLayers() noexcept
{
	ReleaseSceneLayerGroups(false);
	for (auto& item : _sceneLayers)
		ReleaseSceneLayerResources(item);
	_sceneLayers.clear();
	_sceneCommandRecorder.reset();
}

void PresentationRenderHost::ReleaseSceneLayerResources(
	SceneLayer& item) noexcept
{
	const bool materialized = item.Visual || item.Graphics;
	item.Recorder = nullptr;
	AccumulateSceneLayerSubmittedSnapshotStatistics(item);
	item.Graphics.reset();
	if (item.EffectGroup)
	{
		if (item.ContentVisual) (void)item.ContentVisual->SetEffect(nullptr);
		item.EffectGroup->Release();
		item.EffectGroup = nullptr;
	}
	if (item.Clip)
	{
		if (item.Visual) (void)item.Visual->SetClip(nullptr);
		item.Clip->Release();
		item.Clip = nullptr;
	}
	for (auto* clip : item.IntermediateClips)
		if (clip) clip->Release();
	item.IntermediateClips.clear();
	for (auto* visual : item.IntermediateClipVisuals)
		if (visual) visual->Release();
	item.IntermediateClipVisuals.clear();
	if (item.Visual)
	{
		if (_composition) _composition->DestroyD2DLayer(item.Visual);
		item.Visual->Release();
		item.Visual = nullptr;
	}
	if (item.ContentVisual)
	{
		item.ContentVisual->Release();
		item.ContentVisual = nullptr;
	}
	item.VisualProperties = {};
	item.SurfaceProperties = {};
	item.Layer = 0;
	item.Order = 0;
	item.GroupIndex = SceneLayerGroupProperties::NoParent;
	item.CreatedInTopologyBatch = false;
	item.UsesCompositionSurface = false;
	if (materialized) ++_sceneLayerReleaseCount;
}

void PresentationRenderHost::DiscardSceneLayerTopologyBatch(
	size_t initialSlotCount) noexcept
{
	bool changed = false;
	for (auto& item : _sceneLayers)
	{
		if (!item.CreatedInTopologyBatch) continue;
		ReleaseSceneLayerResources(item);
		changed = true;
	}
	if (_sceneLayers.size() > initialSlotCount)
	{
		_sceneLayers.resize(initialSlotCount);
		changed = true;
	}
	if (SceneLayerCount() == 0u) _sceneCommandRecorder.reset();
	_sceneLayerBatchResourceMutationPending = false;
	if (changed)
	{
		AdvanceResourceGeneration();
		InvalidateFrameHistory();
	}
}

void PresentationRenderHost::AcceptSceneLayerTopologyBatch() noexcept
{
	for (auto& item : _sceneLayers)
		item.CreatedInTopologyBatch = false;
	if (_sceneLayerBatchResourceMutationPending)
	{
		UpdateSceneResourcePeaks();
		AdvanceResourceGeneration();
		InvalidateFrameHistory();
		_sceneLayerBatchResourceMutationPending = false;
	}
}

void PresentationRenderHost::ReleaseSceneLayerGroups(
	bool restoreChildren,
	IDCompositionVisual* skipNativeVisual) noexcept
{
#ifdef CUI_ENABLE_WEBVIEW2
	for (auto& layer : _sceneLayers)
	{
		if (layer.GroupIndex == SceneLayerGroupProperties::NoParent) continue;
		const size_t owner = layer.GroupIndex;
		if (owner < _sceneLayerGroups.size()
			&& _sceneLayerGroups[owner].ContentVisual && layer.Visual)
			(void)_sceneLayerGroups[owner].ContentVisual->RemoveVisual(
				layer.Visual);
		layer.GroupIndex = SceneLayerGroupProperties::NoParent;
		if (restoreChildren && _composition && layer.Visual
			&& !_composition->RegisterVisual(
				layer.Visual, layer.Layer, layer.Order))
			_deviceResetRequested = true;
	}
	for (auto& group : _sceneLayerGroups)
	{
		for (const auto& native : group.Properties.NativeVisuals)
		{
			if (group.ContentVisual && native.Visual)
				(void)group.ContentVisual->RemoveVisual(native.Visual);
			if (restoreChildren && _composition && native.Visual
				&& native.Visual != skipNativeVisual
				&& !_composition->RegisterVisual(
					native.Visual, group.Properties.Layer, native.Order))
				_deviceResetRequested = true;
		}
	}
	for (size_t groupIndex = 0;
		groupIndex < _sceneLayerGroups.size(); ++groupIndex)
	{
		auto& group = _sceneLayerGroups[groupIndex];
		if (group.Properties.ParentGroup
			!= SceneLayerGroupProperties::NoParent
			&& group.Properties.ParentGroup < _sceneLayerGroups.size())
		{
			auto* parent = _sceneLayerGroups[
				group.Properties.ParentGroup].ContentVisual;
			if (parent && group.Visual)
				(void)parent->RemoveVisual(group.Visual);
		}
		else if (group.Visual && _composition)
			_composition->UnregisterVisual(group.Visual);
	}
	for (size_t reverseIndex = _sceneLayerGroups.size();
		reverseIndex > 0u; --reverseIndex)
	{
		auto& group = _sceneLayerGroups[reverseIndex - 1u];
		if (group.Visual)
		{
			(void)group.Visual->SetClip(nullptr);
		}
		if (group.ContentVisual)
			(void)group.ContentVisual->SetEffect(nullptr);
		auto* contentParent = group.IntermediateClipVisuals.empty()
			? group.Visual : group.IntermediateClipVisuals.back();
		if (contentParent && group.ContentVisual)
			(void)contentParent->RemoveVisual(group.ContentVisual);
		if (group.Visual && !group.IntermediateClipVisuals.empty())
			(void)group.Visual->RemoveVisual(
				group.IntermediateClipVisuals.front());
		for (size_t index = 0;
			index < group.IntermediateClipVisuals.size(); ++index)
		{
			auto* visual = group.IntermediateClipVisuals[index];
			if (visual) (void)visual->SetClip(nullptr);
			if (visual && index + 1u < group.IntermediateClipVisuals.size())
				(void)visual->RemoveVisual(
					group.IntermediateClipVisuals[index + 1u]);
			if (index < group.IntermediateClips.size()
				&& group.IntermediateClips[index])
				group.IntermediateClips[index]->Release();
			if (visual) visual->Release();
		}
		group.IntermediateClipVisuals.clear();
		group.IntermediateClips.clear();
		if (group.Clip)
		{
			group.Clip->Release();
			group.Clip = nullptr;
		}
		if (group.ContentVisual)
		{
			group.ContentVisual->Release();
			group.ContentVisual = nullptr;
		}
		if (group.Visual)
		{
			group.Visual->Release();
			group.Visual = nullptr;
		}
		if (group.EffectGroup)
		{
			group.EffectGroup->Release();
			group.EffectGroup = nullptr;
		}
	}
	_sceneLayerGroups.clear();
	if (_deviceResetRequested) InvalidateFrameHistory();
#else
	(void)restoreChildren;
	(void)skipNativeVisual;
	_sceneLayerGroups.clear();
#endif
}

IDCompositionDevice* PresentationRenderHost::CompositionDevice() const noexcept
{
	return _composition ? _composition->GetDCompDevice() : nullptr;
}

IDCompositionVisual* PresentationRenderHost::WebContainerVisual() const noexcept
{
	return _composition ? _composition->GetWebContainerVisual() : nullptr;
}

size_t PresentationRenderHost::
SceneLayerGroupedNativeVisualCountForTesting() const noexcept
{
	size_t result = 0;
	for (const auto& group : _sceneLayerGroups)
		result += group.Properties.NativeVisuals.size();
	return result;
}

bool PresentationRenderHost::RegisterCompositionVisual(
	IDCompositionVisual* visual,
	int layer,
	int order)
{
	for (const auto& group : _sceneLayerGroups)
		if (std::any_of(
			group.Properties.NativeVisuals.begin(),
			group.Properties.NativeVisuals.end(),
			[visual](const SceneLayerGroupProperties::NativeVisual& native)
			{ return native.Visual == visual; })) return true;
	return _composition
		&& _composition->RegisterVisual(visual, layer, order);
}

void PresentationRenderHost::UpdateCompositionVisualOrder(
	IDCompositionVisual* visual,
	int layer,
	int order)
{
	for (const auto& group : _sceneLayerGroups)
	{
		const auto found = std::find_if(
			group.Properties.NativeVisuals.begin(),
			group.Properties.NativeVisuals.end(),
			[visual](const SceneLayerGroupProperties::NativeVisual& native)
			{ return native.Visual == visual; });
		if (found == group.Properties.NativeVisuals.end()) continue;
		// While leased, the group owns the visual's global parent and layer.
		// Native owners still publish their ordinary content-layer preference;
		// only a scene-order change invalidates the lease topology.
		if (found->Order == order) return;
		ReleaseSceneLayerGroups(true);
		AdvanceResourceGeneration();
		InvalidateFrameHistory();
		if (_transactionOpen) _deviceResetRequested = true;
		break;
	}
	if (_composition) _composition->UpdateVisualOrder(visual, layer, order);
}

void PresentationRenderHost::UnregisterCompositionVisual(
	IDCompositionVisual* visual)
{
	bool grouped = false;
	for (const auto& group : _sceneLayerGroups)
	{
		grouped = std::any_of(
			group.Properties.NativeVisuals.begin(),
			group.Properties.NativeVisuals.end(),
			[visual](const SceneLayerGroupProperties::NativeVisual& native)
			{ return native.Visual == visual; });
		if (grouped) break;
	}
	if (grouped)
	{
		ReleaseSceneLayerGroups(true, visual);
		AdvanceResourceGeneration();
		InvalidateFrameHistory();
		if (_transactionOpen) _deviceResetRequested = true;
	}
	if (_composition) _composition->UnregisterVisual(visual);
}

bool PresentationRenderHost::CommitComposition() noexcept
{
	if (!_composition) return true;
	// Native visuals may request a commit while their node is being submitted.
	// The active frame transaction owns the single atomic commit point.
	if (_transactionOpen) return true;
	const HRESULT result = _composition->CommitComposition();
	if (SUCCEEDED(result)) return true;
	_deviceResetRequested = true;
	InvalidateFrameHistory();
	return false;
}

void PresentationRenderHost::AdvanceResourceGeneration() noexcept
{
	if (++_resourceGeneration == 0) ++_resourceGeneration;
}

void PresentationRenderHost::AdvanceDeviceResourceGeneration() noexcept
{
	if (++_deviceResourceGeneration == 0) ++_deviceResourceGeneration;
}

void PresentationRenderHost::UpdateSceneResourcePeaks() noexcept
{
	ScopedMicrosecondAccumulator timing(
		_sceneLayerResourcePeakUpdateMicroseconds);
	_peakSceneLayerCount = (std::max)(
		_peakSceneLayerCount, SceneLayerCount());
	_peakSceneLayerSlotCount = (std::max)(
		_peakSceneLayerSlotCount, _sceneLayers.size());
	uint64_t swapChainBytes = 0;
	uint64_t surfaceBytes = 0;
	uint64_t snapshotBytes = 0;
	for (const auto& layer : _sceneLayers)
	{
		if (!layer.Visual || !layer.Graphics) continue;
		if (layer.UsesCompositionSurface)
		{
			surfaceBytes = SaturatingAdd(surfaceBytes, EstimatedSurfaceBytes(
				layer.SurfaceProperties.PhysicalWidth,
				layer.SurfaceProperties.PhysicalHeight));
			snapshotBytes = SaturatingAdd(snapshotBytes,
				static_cast<const CompositionSurfaceGraphics*>(
					layer.Graphics.get())->SubmittedTextureBytes());
		}
		else
			swapChainBytes = SaturatingAdd(
				swapChainBytes, EstimatedSwapChainBytes(
					layer.SurfaceProperties.PhysicalWidth,
					layer.SurfaceProperties.PhysicalHeight));
	}
	_peakEstimatedSceneSwapChainBytes = (std::max)(
		_peakEstimatedSceneSwapChainBytes, swapChainBytes);
	_peakEstimatedSceneCompositionSurfaceBytes = (std::max)(
		_peakEstimatedSceneCompositionSurfaceBytes, surfaceBytes);
	_peakEstimatedSceneSubmittedSnapshotBytes = (std::max)(
		_peakEstimatedSceneSubmittedSnapshotBytes, snapshotBytes);
	const uint64_t slotBytes = SaturatingMultiply(
		static_cast<uint64_t>(_sceneLayers.capacity()),
		static_cast<uint64_t>(sizeof(SceneLayer)));
	_peakEstimatedSceneLayerSlotBytes = (std::max)(
		_peakEstimatedSceneLayerSlotBytes, slotBytes);
}

void PresentationRenderHost::AccumulateSceneLayerSubmittedSnapshotStatistics(
	const SceneLayer& layer) noexcept
{
	if (!layer.UsesCompositionSurface || !layer.Graphics) return;
	const auto value = static_cast<const CompositionSurfaceGraphics*>(
		layer.Graphics.get())->SubmittedSnapshotStats();
	_sceneLayerSubmittedSnapshotCreateCount += value.CreateCount;
	_sceneLayerSubmittedSnapshotUpdateCount += value.UpdateCount;
	_sceneLayerSubmittedSnapshotCopiedBytes += value.CopiedBytes;
	_sceneLayerSubmittedSnapshotCreateMicroseconds += value.CreateMicroseconds;
	_sceneLayerSubmittedSnapshotCopyMicroseconds += value.CopyMicroseconds;
}
