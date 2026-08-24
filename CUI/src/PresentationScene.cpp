#include "PresentationScene.h"

#include "Control.h"
#include "DCompLayeredHost.h"
#include "PresentationInfrastructure.h"
#include "PresentationRenderHost.h"
#include "Graphics.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_set>
#include <utility>

namespace
{
	class SceneWorkClock final
	{
	public:
		SceneWorkClock() noexcept
		{
			(void)::QueryPerformanceCounter(&_start);
		}

		double ElapsedMicroseconds() const noexcept
		{
			LARGE_INTEGER now{};
			(void)::QueryPerformanceCounter(&now);
			static const LONGLONG frequency = []() noexcept
			{
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

	class SceneWorkAccumulator final
	{
	public:
		explicit SceneWorkAccumulator(double& destination) noexcept
			: _destination(destination)
		{
		}

		~SceneWorkAccumulator() noexcept
		{
			_destination += _clock.ElapsedMicroseconds();
		}

	private:
		double& _destination;
		SceneWorkClock _clock;
	};

	void AdvanceRevision(uint64_t& value) noexcept
	{
		++value;
		if (value == 0) ++value;
	}

	bool RectIntersects(const RECT& left, const RECT& right) noexcept
	{
		return left.left < right.right && left.right > right.left
			&& left.top < right.bottom && left.bottom > right.top;
	}

	RECT ToRect(D2D1_RECT_F value, int inflate = 0) noexcept
	{
		return RECT{
			static_cast<LONG>(std::floor(value.left)) - inflate,
			static_cast<LONG>(std::floor(value.top)) - inflate,
			static_cast<LONG>(std::ceil(value.right)) + inflate,
			static_cast<LONG>(std::ceil(value.bottom)) + inflate };
	}

	D2D1::Matrix3x2F AsMatrix(D2D1_MATRIX_3X2_F value) noexcept
	{
		return D2D1::Matrix3x2F(
			value._11, value._12, value._21,
			value._22, value._31, value._32);
	}

	bool SameMatrix(
		const D2D1_MATRIX_3X2_F& left,
		const D2D1_MATRIX_3X2_F& right) noexcept
	{
		return left._11 == right._11 && left._12 == right._12
			&& left._21 == right._21 && left._22 == right._22
			&& left._31 == right._31 && left._32 == right._32;
	}

	bool FiniteRect(const D2D1_RECT_F& value) noexcept
	{
		return std::isfinite(value.left) && std::isfinite(value.top)
			&& std::isfinite(value.right) && std::isfinite(value.bottom)
			&& value.right >= value.left && value.bottom >= value.top;
	}

	bool SameRect(
		const D2D1_RECT_F& left,
		const D2D1_RECT_F& right) noexcept
	{
		return left.left == right.left && left.top == right.top
			&& left.right == right.right && left.bottom == right.bottom;
	}

	D2D1_RECT_F TransformBounds(
		D2D1_RECT_F value,
		D2D1_MATRIX_3X2_F transform) noexcept
	{
		const auto matrix = AsMatrix(transform);
		const D2D1_POINT_2F points[]{
			matrix.TransformPoint(D2D1::Point2F(value.left, value.top)),
			matrix.TransformPoint(D2D1::Point2F(value.right, value.top)),
			matrix.TransformPoint(D2D1::Point2F(value.left, value.bottom)),
			matrix.TransformPoint(D2D1::Point2F(value.right, value.bottom)) };
		D2D1_RECT_F result{
			points[0].x, points[0].y, points[0].x, points[0].y };
		for (size_t index = 1; index < std::size(points); ++index)
		{
			result.left = (std::min)(result.left, points[index].x);
			result.top = (std::min)(result.top, points[index].y);
			result.right = (std::max)(result.right, points[index].x);
			result.bottom = (std::max)(result.bottom, points[index].y);
		}
		return result;
	}

	bool GetClientClip(
		Control* control,
		const RECT& contentDirty,
		int titleBarOffset,
		RECT& result)
	{
		if (!control) return false;
		result = contentDirty;
		result.top += titleBarOffset;
		result.bottom += titleBarOffset;

		// Popup and other transient presentation roots render in the window
		// viewport.  Their former logical ancestors must not clip the retained
		// scene; descendants still honor clips inside the transient subtree.
		for (Control* current =
			cui::framework::PresentationAccess::
			BreaksVisualPresentationInheritance(*control)
			? nullptr : control->GetVisualParent(); current;)
		{
			if (current->ClipsChildren())
			{
				auto clip = current->GetVisualChildrenClipRect();
				const auto absolute = current->GetAbsoluteLocationDip();
				clip = current->TransformAbsoluteRectToRenderSpace(D2D1_RECT_F{
					clip.left + absolute.x,
					clip.top + absolute.y,
					clip.right + absolute.x,
					clip.bottom + absolute.y });
				RECT clipRect{
					static_cast<LONG>(std::floor(clip.left)),
					static_cast<LONG>(std::floor(clip.top)) + titleBarOffset,
					static_cast<LONG>(std::ceil(clip.right)),
					static_cast<LONG>(std::ceil(clip.bottom)) + titleBarOffset };
				RECT intersection{};
				if (!::IntersectRect(&intersection, &result, &clipRect))
					return false;
				result = intersection;
			}

			if (cui::framework::PresentationAccess::
				BreaksVisualPresentationInheritance(*current)) break;
			current = current->GetVisualParent();
		}
		return result.right > result.left && result.bottom > result.top;
	}

	struct AncestorRectangleClip
	{
		D2D1_RECT_F LocalRect{};
		float RadiusX = 0.0f;
		float RadiusY = 0.0f;
		D2D1_MATRIX_3X2_F LocalToRoot{
			1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
	};
	struct AncestorGeometryClip
	{
		cui::drawing::Geometry Value;
		D2D1_MATRIX_3X2_F LocalToRoot{
			1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
	};

	bool TryResolveAncestorClips(
		Control* control,
		std::vector<AncestorRectangleClip>& chain,
		std::vector<AncestorGeometryClip>& geometryClips,
		bool& canFlatten,
		bool& hasClip,
		D2D1_RECT_F& result,
		Control* exclusiveBoundary = nullptr,
		bool includeControl = false)
	{
		chain.clear();
		geometryClips.clear();
		canFlatten = true;
		hasClip = false;
		result = {};
		if (!control) return false;
		auto addBounds = [&](D2D1_RECT_F clip)
		{
			if (!std::isfinite(clip.left) || !std::isfinite(clip.top)
				|| !std::isfinite(clip.right) || !std::isfinite(clip.bottom)
				|| clip.right <= clip.left || clip.bottom <= clip.top)
				return false;
			if (!hasClip)
			{
				result = clip;
				hasClip = true;
				return true;
			}
			result.left = (std::max)(result.left, clip.left);
			result.top = (std::max)(result.top, clip.top);
			result.right = (std::min)(result.right, clip.right);
			result.bottom = (std::min)(result.bottom, clip.bottom);
			// Empty ancestor intersections are valid zero coverage. Rejecting one
			// aborts the unrelated Window frame during viewport resizes.
			result.right = (std::max)(result.right, result.left);
			result.bottom = (std::max)(result.bottom, result.top);
			return true;
		};
		auto addClip = [&](D2D1_RECT_F localClip,
			float radiusX,
			float radiusY,
			D2D1_MATRIX_3X2_F localToRoot,
			bool flattenable)
		{
			if (!std::isfinite(localClip.left)
				|| !std::isfinite(localClip.top)
				|| !std::isfinite(localClip.right)
				|| !std::isfinite(localClip.bottom)
				|| localClip.right <= localClip.left
				|| localClip.bottom <= localClip.top
				|| !std::isfinite(radiusX) || radiusX < 0.0f
				|| !std::isfinite(radiusY) || radiusY < 0.0f
				|| !std::isfinite(localToRoot._11)
				|| !std::isfinite(localToRoot._12)
				|| !std::isfinite(localToRoot._21)
				|| !std::isfinite(localToRoot._22)
				|| !std::isfinite(localToRoot._31)
				|| !std::isfinite(localToRoot._32)) return false;
			chain.push_back({ localClip, radiusX, radiusY, localToRoot });
			const auto clip = TransformBounds(localClip, localToRoot);
			canFlatten = canFlatten && flattenable
				&& radiusX == 0.0f && radiusY == 0.0f
				&& std::abs(localToRoot._12) <= 1.0e-6f
				&& std::abs(localToRoot._21) <= 1.0e-6f;
			return addBounds(clip);
		};
		if (!includeControl && control == exclusiveBoundary) return true;
	// A Popup or ContextMenu owns a window-viewport presentation space. Its
	// logical visual parent remains connected for templating and input routing,
	// but cannot contribute clips to this independent composition surface.
	if (!includeControl && cui::framework::PresentationAccess::
		BreaksVisualPresentationInheritance(*control)) return true;
		for (auto* ancestor = includeControl
			? control : control->GetVisualParent(); ancestor;)
		{
			if (ancestor == exclusiveBoundary) break;
			if (const auto& explicitClip = ancestor->GetClip())
			{
				D2D1_RECT_F localClip{};
				float radiusX = 0.0f;
				float radiusY = 0.0f;
				bool isGeometryMask = false;
				if (explicitClip->Kind
					== cui::drawing::GeometryKind::Rectangle)
				{
					localClip = D2D1::RectF(
						(std::min)(explicitClip->Rect.left,
							explicitClip->Rect.right),
						(std::min)(explicitClip->Rect.top,
							explicitClip->Rect.bottom),
						(std::max)(explicitClip->Rect.left,
							explicitClip->Rect.right),
						(std::max)(explicitClip->Rect.top,
							explicitClip->Rect.bottom));
					const float halfWidth =
						(localClip.right - localClip.left) * 0.5f;
					const float halfHeight =
						(localClip.bottom - localClip.top) * 0.5f;
					if (!std::isfinite(explicitClip->RadiusX)
						|| !std::isfinite(explicitClip->RadiusY)
						|| explicitClip->RadiusX < 0.0f
						|| explicitClip->RadiusY < 0.0f) return false;
					radiusX = (std::min)(explicitClip->RadiusX, halfWidth);
					radiusY = (std::min)(explicitClip->RadiusY, halfHeight);
					if (radiusX <= 0.0f || radiusY <= 0.0f)
						radiusX = radiusY = 0.0f;
				}
				else if (explicitClip->Kind
					== cui::drawing::GeometryKind::Ellipse)
				{
					if (!std::isfinite(explicitClip->Center.x)
						|| !std::isfinite(explicitClip->Center.y)
						|| !std::isfinite(explicitClip->RadiusX)
						|| !std::isfinite(explicitClip->RadiusY)
						|| explicitClip->RadiusX <= 0.0f
						|| explicitClip->RadiusY <= 0.0f) return false;
					radiusX = explicitClip->RadiusX;
					radiusY = explicitClip->RadiusY;
					localClip = D2D1::RectF(
						explicitClip->Center.x - radiusX,
						explicitClip->Center.y - radiusY,
						explicitClip->Center.x + radiusX,
						explicitClip->Center.y + radiusY);
				}
				else if (explicitClip->Kind == cui::drawing::GeometryKind::Path
					|| explicitClip->Kind == cui::drawing::GeometryKind::Group)
				{
					const auto ownerToRoot = ancestor->GetLocalToRenderTransform();
					Microsoft::WRL::ComPtr<ID2D1Geometry> native;
					native.Attach(explicitClip->CreateD2DGeometry(&ownerToRoot));
					D2D1_RECT_F bounds{};
					if (!native || FAILED(native->GetBounds(nullptr, &bounds))
						|| !addBounds(bounds)) return false;
					geometryClips.push_back({ *explicitClip, ownerToRoot });
					canFlatten = false;
					isGeometryMask = true;
				}
				else return false;
				if (!isGeometryMask)
				{
					auto localToRoot = AsMatrix(
						ancestor->GetLocalToRenderTransform());
					if (explicitClip->LocalTransform
						&& !explicitClip->LocalTransform->Empty())
						localToRoot = AsMatrix(
							explicitClip->LocalTransform->ToMatrix(
								D2D1::SizeF())) * localToRoot;
					if (!addClip(localClip, radiusX, radiusY,
						localToRoot, false)) return false;
				}
			}
			if (ancestor->ClipsChildren())
			{
				const auto localClip = ancestor->GetVisualChildrenClipRect();
				const auto localToRoot = ancestor->GetLocalToRenderTransform();
				if (!addClip(localClip, 0.0f, 0.0f, localToRoot, true))
					return false;
			}
			if (cui::framework::PresentationAccess::
				BreaksVisualPresentationInheritance(*ancestor)) break;
			ancestor = ancestor->GetVisualParent();
		}
		std::reverse(chain.begin(), chain.end());
		std::reverse(geometryClips.begin(), geometryClips.end());
		return true;
	}

	std::vector<Control*> SortedVisibleChildren(Control* control)
	{
		auto children = control ? control->GetVisualChildrenInZOrder()
			: std::vector<Control*>{};
		children.erase(
			std::remove_if(children.begin(), children.end(), [](Control* child)
			{
				return !child || !child->IsVisible
					|| !child->ParticipatesInPresentationScene();
			}),
			children.end());
		return children;
	}
}

void PresentationScene::InvalidateNode(
	Control* control,
	PresentationInvalidationKind kind) noexcept
{
	if (!control || kind == PresentationInvalidationKind::None) return;
	if (HasPresentationInvalidation(
		kind, PresentationInvalidationKind::Content))
		AdvanceRevision(_contentRevision);
	if (HasPresentationInvalidation(
		kind, PresentationInvalidationKind::Geometry))
		AdvanceRevision(_geometryRevision);
	if (HasPresentationInvalidation(
		kind, PresentationInvalidationKind::Composition))
		AdvanceRevision(_compositionRevision);

	if (_structureDirty) return;
	const auto found = _nodeIndex.find(control);
	if (found == _nodeIndex.end() || found->second >= _nodes.size()) return;
	auto& node = _nodes[found->second];
	if (HasPresentationInvalidation(
		kind, PresentationInvalidationKind::Content))
		node.ContentDirty = true;
	if (HasPresentationInvalidation(
		kind, PresentationInvalidationKind::Composition))
		node.CompositionDirty = true;
	if (HasPresentationInvalidation(
		kind, PresentationInvalidationKind::Geometry))
	{
		const size_t end = (std::min)(node.SubtreeEnd, _nodes.size());
		try
		{
			_pendingGeometryRanges.push_back(PendingGeometryRange{
				found->second, end,
				HasPresentationInvalidation(
					kind, PresentationInvalidationKind::Transform) });
		}
		catch (...)
		{
			// Allocation failure may cost one full geometry pass, never correctness.
			_allGeometryDirty = true;
			_pendingGeometryRanges.clear();
		}
	}
}

bool PresentationScene::Synchronize(
	std::span<Control* const> roots,
	std::span<Control* const> transientRoots,
	std::span<Control* const> compositionIsolationTargets,
	std::span<Control* const> opacityIsolationTargets)
{
	if (_transientRoots.size() != transientRoots.size()
		|| !std::equal(
			_transientRoots.begin(), _transientRoots.end(),
			transientRoots.begin(), transientRoots.end()))
	{
		_transientRoots.assign(transientRoots.begin(), transientRoots.end());
		_structureDirty = true;
	}
	auto updateWeakTargets = [this](
		std::vector<ControlWeakReference>& current,
		std::span<Control* const> requested)
	{
		bool changed = current.size() != requested.size();
		if (!changed)
		{
			for (size_t index = 0; index < requested.size(); ++index)
			{
				if (current[index].Get() != requested[index])
				{
					changed = true;
					break;
				}
			}
		}
		if (!changed) return;
		std::vector<ControlWeakReference> replacement;
		replacement.reserve(requested.size());
		for (auto* target : requested)
			if (target) replacement.emplace_back(target);
		current = std::move(replacement);
		_structureDirty = true;
	};
	updateWeakTargets(
		_compositionIsolationTargets, compositionIsolationTargets);
	updateWeakTargets(_opacityIsolationTargets, opacityIsolationTargets);
	if (!_structureDirty || _rendering) return false;
	Rebuild(roots);
	_structureDirty = false;
	AdvanceRevision(_revision);
	return true;
}

void PresentationScene::SynchronizeResourceGeneration(
	uint64_t generation) noexcept
{
	if (generation == 0 || generation == _resourceGeneration) return;
	_resourceGeneration = generation;
	for (auto& node : _nodes)
	{
		if (node.DrawingCommands)
		{
			node.DrawingCommands.Reset();
			node.CommandGeneration = 0;
			++_pendingCommandCacheInvalidations;
		}
		node.HasPresented = false;
	}
	_overlaySurfaceDirty = true;
}

void PresentationScene::Rebuild(std::span<Control* const> roots)
{
	// The transparent swap chain retains pixels independently of the primary
	// surface.  A topology change must therefore produce one clearing frame even
	// when the last transient root was removed and no overlay nodes remain.
	_overlaySurfaceDirty = true;
	_rasterRoots.assign(roots.begin(), roots.end());
	std::stable_sort(
		_rasterRoots.begin(), _rasterRoots.end(),
		[](Control* left, Control* right)
		{
			if (!left || !right) return left != nullptr;
			return left->ZIndex < right->ZIndex;
		});
	_rasterRoots.erase(
		std::remove_if(
			_rasterRoots.begin(), _rasterRoots.end(),
			[this](Control* control)
			{
				if (!control || !control->IsVisible) return true;
				return std::find(
					_transientRoots.begin(), _transientRoots.end(), control)
					!= _transientRoots.end();
			}),
		_rasterRoots.end());

	_pendingCommandCacheInvalidations += static_cast<size_t>(std::count_if(
		_nodes.begin(), _nodes.end(), [](const Node& node)
		{ return node.DrawingCommands != nullptr; }));
	_nodes.clear();
	_segments.clear();
	_opacityGroups.clear();
	_nodeIndex.clear();
	_pendingGeometryRanges.clear();
	_allGeometryDirty = false;
	const size_t expectedNodeCount = _rasterRoots.size()
		+ _transientRoots.size()
		+ _compositionIsolationTargets.size()
		+ _opacityIsolationTargets.size();
	_nodes.reserve(expectedNodeCount);
	_nodeIndex.reserve(expectedNodeCount);
	// A transient root can use an independent retained surface only when a DComp
	// host exists. EnsureComposition may still fail at runtime, in which case the
	// ordinary shared-overlay raster traversal remains authoritative.
	_requiresComposition = !_transientRoots.empty();
	int order = 0;
	auto isTransientRoot = [this](Control* control)
	{
		return std::find(
			_transientRoots.begin(), _transientRoots.end(), control)
			!= _transientRoots.end();
	};
	std::function<void(Control*, bool, Control*)> visit =
		[&](Control* control, bool overlay, Control* transientRoot)
	{
		if (!control || !control->IsVisible) return;
		// Popup can be authored inside another transient root (for example a
		// ContextMenu MenuItem submenu).  It is nevertheless an independent WPF
		// presentation boundary.  Visiting it through the parent root as well as
		// through _transientRoots nests its surface under the parent's small clip:
		// hit testing still sees the absolute Popup bounds, but its pixels disappear.
		// Prune every foreign transient root from the current traversal and visit it
		// exactly once from its own top-level transient slot below.
		if (isTransientRoot(control) && control != transientRoot) return;
		if (_nodeIndex.contains(control)) return;
		const bool native = control->GetPresentationSurfaceKind()
			== PresentationSurfaceKind::NativeComposition;
		if (native) _requiresComposition = true;
		const size_t nodeIndex = _nodes.size();
		Node node;
		node.Element = control;
		node.Order = order++;
		node.NativeComposition = native;
		node.Overlay = overlay;
		node.AppliedRevisions = {};
		_nodes.push_back(std::move(node));
		_nodeIndex.emplace(control, nodeIndex);
		for (auto* child : SortedVisibleChildren(control))
			visit(child, overlay, transientRoot);
		_nodes[nodeIndex].SubtreeEnd = _nodes.size();
	};
	for (auto* root : _rasterRoots) visit(root, false, nullptr);
	for (auto* root : _transientRoots) visit(root, true, root);

	std::unordered_set<Control*> requestedTargets;
	std::unordered_set<Control*> opacityTargets;
	requestedTargets.reserve(_compositionIsolationTargets.size()
		+ _opacityIsolationTargets.size() + _nodes.size());
	opacityTargets.reserve(_opacityIsolationTargets.size() + _nodes.size());
	for (const auto& target : _compositionIsolationTargets)
		if (auto* live = target.Get()) requestedTargets.insert(live);
	for (const auto& target : _opacityIsolationTargets)
		if (auto* live = target.Get())
		{
			requestedTargets.insert(live);
			opacityTargets.insert(live);
		}
	// Every transient root is an isolation candidate even at opacity 1. This is
	// the ownership boundary that replaces the unsafe whole-overlay reparent:
	// each accepted root receives its own retained surface/global visual slot.
	for (auto* root : _transientRoots)
		if (root)
		{
			requestedTargets.insert(root);
			opacityTargets.insert(root);
		}
	for (const auto& node : _nodes)
	{
		auto* control = node.Element.Get();
		if (control && control->GetOpacity() != 1.0)
		{
			requestedTargets.insert(control);
			opacityTargets.insert(control);
		}
	}
	std::vector<size_t> candidates;
	candidates.reserve((std::min)(requestedTargets.size(), _nodes.size()));
	for (size_t index = 0; index < _nodes.size(); ++index)
	{
		auto* control = _nodes[index].Element.Get();
		if (control && requestedTargets.contains(control))
			candidates.push_back(index);
	}
	std::vector<bool> eligible(_nodes.size(), false);
	std::vector<bool> candidateHasAncestorClip(_nodes.size(), false);
	std::vector<bool> candidateRasterizesAncestorGeometryClip(
		_nodes.size(), false);
	std::vector<D2D1_RECT_F> candidateAncestorClips(_nodes.size());
	std::vector<std::vector<Segment::AncestorGeometryClip>>
		candidateAncestorGeometryClips(_nodes.size());
	for (const auto candidate : candidates)
	{
		const auto end = (std::min)(
			_nodes[candidate].SubtreeEnd, _nodes.size());
		// Every structurally valid target participates independently. Nested
		// targets are flattened into adjacent scene segments below; each segment
		// records with its entire outer-to-inner target chain suppressed and then
		// publishes the combined base-to-live transform exactly once.
		auto* isolationRoot = _nodes[candidate].Element.Get();
		const bool overlayCandidate = _nodes[candidate].Overlay;
		bool valid = !_nodes[candidate].NativeComposition
			&& end > candidate;
		const bool opacityCandidate =
			opacityTargets.contains(isolationRoot);
		std::vector<AncestorRectangleClip> resolvedAncestorClips;
		std::vector<AncestorGeometryClip> resolvedAncestorGeometryClips;
		bool canFlattenAncestorClips = true;
		bool hasAncestorClip = false;
		D2D1_RECT_F ancestorClip{};
		if (valid && !TryResolveAncestorClips(
			isolationRoot, resolvedAncestorClips,
			resolvedAncestorGeometryClips, canFlattenAncestorClips,
			hasAncestorClip, ancestorClip)) valid = false;
		if (valid && isolationRoot)
		{
			cui::framework::PresentationAccess::
				RenderTransformSuppressionScope suppress(*isolationRoot);
			auto base = AsMatrix(
				cui::framework::PresentationAccess::
					LocalToRenderTransformForRecording(*isolationRoot));
			valid = base.Invert();
			bool hasFiniteArea = false;
			for (size_t index = candidate; valid && index < end; ++index)
			{
				auto* item = _nodes[index].Element.Get();
				if (!item || !item->IsVisible) continue;
				const auto bounds = cui::framework::PresentationAccess::
					RenderedAbsoluteRectForRecording(*item);
				valid = std::isfinite(bounds.left)
					&& std::isfinite(bounds.top)
					&& std::isfinite(bounds.right)
					&& std::isfinite(bounds.bottom);
				if (valid && bounds.right > bounds.left
					&& bounds.bottom > bounds.top)
					hasFiniteArea = true;
			}
			valid = valid && hasFiniteArea;
		}
		for (size_t index = candidate; valid && index < end; ++index)
			if (_nodes[index].Overlay != overlayCandidate
				|| (_nodes[index].NativeComposition
					&& (!opacityCandidate
						|| !_nodes[index].Element.Get()
						|| !_nodes[index].Element.Get()->
							SupportsNativeCompositionVisualLease())))
				valid = false;
		eligible[candidate] = valid;
		candidateHasAncestorClip[candidate] = valid && hasAncestorClip;
		candidateRasterizesAncestorGeometryClip[candidate] =
			valid && !resolvedAncestorGeometryClips.empty();
		if (valid && !resolvedAncestorGeometryClips.empty())
		{
			auto& accepted = candidateAncestorGeometryClips[candidate];
			accepted.reserve(resolvedAncestorGeometryClips.size());
			for (const auto& clip : resolvedAncestorGeometryClips)
				accepted.push_back({ clip.Value, clip.LocalToRoot });
		}
		if (valid && hasAncestorClip)
			candidateAncestorClips[candidate] = ancestorClip;
	}
	// A WPF opacity is one group around the complete element subtree. A pure
	// retained scene can preserve that post-composition step by parenting every
	// contiguous subtree segment under one effect visual. Rectangle-family clips
	// common to the group are represented once above that effect. Native visuals
	// participate only through an explicit lease. Transient roots are isolated as
	// one atomic overlay set so their relative order cannot mix with the legacy
	// shared overlay; arbitrary Geometry group clips and invalid chains still
	// fall back. Nested shared roots resolve inner-first and later become a proper
	// parent/child group graph rather than overlapping global siblings.
	std::vector<bool> hasRequestedDescendant(_nodes.size(), false);
	std::vector<bool> hasNativeDescendant(_nodes.size(), false);
	for (size_t reverseIndex = candidates.size();
		reverseIndex > 0u; --reverseIndex)
	{
		const size_t ancestorIndex = reverseIndex - 1u;
		const auto ancestor = candidates[ancestorIndex];
		if (!opacityTargets.contains(_nodes[ancestor].Element.Get())) continue;
		const auto ancestorEnd = (std::min)(
			_nodes[ancestor].SubtreeEnd, _nodes.size());
		bool descendantFound = false;
		for (size_t nodeIndex = ancestor + 1u;
			nodeIndex < ancestorEnd; ++nodeIndex)
			if (_nodes[nodeIndex].NativeComposition)
			{
				hasNativeDescendant[ancestor] = true;
				break;
			}
		for (size_t candidateIndex = ancestorIndex + 1u;
			candidateIndex < candidates.size(); ++candidateIndex)
		{
			const auto candidate = candidates[candidateIndex];
			if (candidate >= ancestorEnd) break;
			descendantFound = true;
		}
		hasRequestedDescendant[ancestor] = descendantFound;
	}
	std::vector<bool> sharedOpacityGroupRoot(_nodes.size(), false);
	for (size_t reverseIndex = candidates.size();
		reverseIndex > 0u; --reverseIndex)
	{
		const size_t ancestorIndex = reverseIndex - 1u;
		const auto ancestor = candidates[ancestorIndex];
		if (!hasRequestedDescendant[ancestor]
			&& !hasNativeDescendant[ancestor]) continue;
		const auto ancestorEnd = (std::min)(
			_nodes[ancestor].SubtreeEnd, _nodes.size());
		auto* root = _nodes[ancestor].Element.Get();
		bool groupEligible = eligible[ancestor] && root;
		std::vector<AncestorRectangleClip> groupClipChain;
		std::vector<AncestorGeometryClip> groupGeometryClips;
		bool groupCanFlatten = true;
		bool groupHasClip = false;
		D2D1_RECT_F groupClip{};
		if (groupEligible
			&& (!TryResolveAncestorClips(root, groupClipChain,
				groupGeometryClips, groupCanFlatten, groupHasClip,
				groupClip, nullptr, true)
				|| !groupGeometryClips.empty())) groupEligible = false;
		for (size_t candidateIndex = ancestorIndex + 1u;
			groupEligible && candidateIndex < candidates.size(); ++candidateIndex)
		{
			const auto candidate = candidates[candidateIndex];
			if (candidate >= ancestorEnd) break;
			if (!eligible[candidate]) groupEligible = false;
		}
		if (groupEligible)
		{
			sharedOpacityGroupRoot[ancestor] = true;
			continue;
		}
		for (size_t candidateIndex = ancestorIndex;
			candidateIndex < candidates.size(); ++candidateIndex)
		{
			const auto candidate = candidates[candidateIndex];
			if (candidate >= ancestorEnd) break;
			eligible[candidate] = false;
			sharedOpacityGroupRoot[candidate] = false;
			candidateHasAncestorClip[candidate] = false;
			candidateAncestorClips[candidate] = {};
		}
	}
	// A descendant cannot safely reuse composition-only commands when a
	// requested animated ancestor fell back to ordinary raster: that ancestor's
	// live transform would otherwise remain baked into the descendant command
	// list. Keep the entire requested chain fail-closed in that mixed case.
	std::vector<size_t> requestedAncestorStack;
	requestedAncestorStack.reserve(8u);
	for (const auto candidate : candidates)
	{
		while (!requestedAncestorStack.empty()
			&& candidate >= (std::min)(
				_nodes[requestedAncestorStack.back()].SubtreeEnd,
				_nodes.size()))
			requestedAncestorStack.pop_back();
		if (eligible[candidate])
		{
			for (const auto ancestor : requestedAncestorStack)
			{
				if (!eligible[ancestor])
				{
					eligible[candidate] = false;
					candidateHasAncestorClip[candidate] = false;
					candidateAncestorClips[candidate] = {};
					break;
				}
			}
		}
		requestedAncestorStack.push_back(candidate);
	}

	// The legacy overlay swap chain is one visual. Mixing one root left on that
	// visual with another independently segmented root cannot preserve their
	// relative order. Segment the complete transient-root set atomically or keep
	// every root on the exact shared-overlay fallback for this topology.
	bool segmentAllTransientRoots = !_transientRoots.empty();
	for (auto* root : _transientRoots)
	{
		const auto found = root ? _nodeIndex.find(root) : _nodeIndex.end();
		if (found == _nodeIndex.end() || found->second >= eligible.size()
			|| !eligible[found->second])
		{
			segmentAllTransientRoots = false;
			break;
		}
	}
	if (!segmentAllTransientRoots)
	{
		for (size_t index = 0; index < _nodes.size(); ++index)
			if (_nodes[index].Overlay)
			{
				eligible[index] = false;
				sharedOpacityGroupRoot[index] = false;
				candidateHasAncestorClip[index] = false;
				candidateAncestorClips[index] = {};
			}
	}

	bool normalSegmentOpen = false;
	size_t currentSegment = NoSegment;
	_segments.reserve(_nodes.size());
	std::vector<size_t> activeIsolationRoots;
	auto matchesActiveIsolationChain = [&](const Segment& segment)
	{
		if (segment.IsolationRoots.size() != activeIsolationRoots.size())
			return false;
		for (size_t chainIndex = 0;
			chainIndex < activeIsolationRoots.size(); ++chainIndex)
		{
			if (segment.IsolationRoots[chainIndex].Get()
				!= _nodes[activeIsolationRoots[chainIndex]].Element.Get())
				return false;
		}
		return true;
	};
	for (size_t index = 0; index < _nodes.size();)
	{
		while (!activeIsolationRoots.empty()
			&& index >= (std::min)(
				_nodes[activeIsolationRoots.back()].SubtreeEnd,
				_nodes.size()))
			activeIsolationRoots.pop_back();
		if (eligible[index]) activeIsolationRoots.push_back(index);
		auto& node = _nodes[index];
		node.OverlaySegmented = node.Overlay
			&& !activeIsolationRoots.empty()
			&& _nodes[activeIsolationRoots.front()].Overlay;
		if (node.NativeComposition
			|| (node.Overlay && !node.OverlaySegmented))
		{
			normalSegmentOpen = false;
			currentSegment = NoSegment;
			++index;
			continue;
		}
		if (!activeIsolationRoots.empty())
		{
			normalSegmentOpen = false;
			if (currentSegment == NoSegment
				|| currentSegment >= _segments.size()
				|| !matchesActiveIsolationChain(_segments[currentSegment]))
			{
				Segment segment;
				segment.Order = node.Order;
				segment.Layer = node.Overlay
					? PresentationSceneOverlayLayer
					: PresentationSceneContentLayer;
				segment.NodeStart = index;
				segment.IsolationRoots.reserve(
					activeIsolationRoots.size());
				for (const auto candidate : activeIsolationRoots)
				{
					segment.IsolationRoots.emplace_back(
						_nodes[candidate].Element.Get());
					if (opacityTargets.contains(
						_nodes[candidate].Element.Get()))
					{
						if (sharedOpacityGroupRoot[candidate])
							segment.OpacityGroupRoots.emplace_back(
								_nodes[candidate].Element.Get());
						else segment.OpacityIsolationRoots.emplace_back(
							_nodes[candidate].Element.Get());
					}
				}
				const auto innermost = activeIsolationRoots.back();
				segment.IsolationRoot = ControlWeakReference(
					_nodes[innermost].Element.Get());
				segment.HasAncestorClip =
					candidateHasAncestorClip[innermost];
				segment.AncestorClipRootDip =
					candidateAncestorClips[innermost];
				segment.RasterizesAncestorGeometryClip =
					candidateRasterizesAncestorGeometryClip[innermost];
				segment.AncestorGeometryClips =
					candidateAncestorGeometryClips[innermost];
				currentSegment = _segments.size();
				_segments.push_back(std::move(segment));
			}
			node.SegmentIndex = currentSegment;
			node.CompositionIsolated = true;
			node.CompositionIsolationRoot = eligible[index];
			_segments[currentSegment].NodeEnd = index + 1u;
			++index;
			continue;
		}
		if (!normalSegmentOpen)
		{
			currentSegment = _segments.size();
			Segment segment;
			segment.Order = node.Order;
			segment.Layer = PresentationSceneContentLayer;
			_segments.push_back(std::move(segment));
			_segments.back().NodeStart = index;
			normalSegmentOpen = true;
		}
		node.SegmentIndex = currentSegment;
		_segments[currentSegment].NodeEnd = index + 1;
		++index;
	}
	// Pure CUI sibling isolation ranges beneath the exact same arbitrary
	// Geometry ancestor already rasterize in fixed root space. When they are
	// contiguous in both retained node order and scene-layer order, one surface
	// can replay each member under its own live logical transform while pushing
	// the common native Geometry once. Exclude opacity, nested isolation,
	// overlay, partial-subtree and native-boundary shapes so z-order and group
	// ownership remain identical to the one-segment-per-target fallback.
	auto exactGeometryChain = [](const Segment& left, const Segment& right)
	{
		if (left.AncestorGeometryClips.size()
			!= right.AncestorGeometryClips.size()) return false;
		for (size_t index = 0;
			index < left.AncestorGeometryClips.size(); ++index)
		{
			const auto& leftClip = left.AncestorGeometryClips[index];
			const auto& rightClip = right.AncestorGeometryClips[index];
			if (!(leftClip.Value == rightClip.Value)
				|| !SameMatrix(
					leftClip.LocalToRootDip,
					rightClip.LocalToRootDip)) return false;
		}
		return true;
	};
	auto exactClipBounds = [](D2D1_RECT_F left, D2D1_RECT_F right)
	{
		return left.left == right.left && left.top == right.top
			&& left.right == right.right && left.bottom == right.bottom;
	};
	auto standaloneGeometrySibling = [this](const Segment& segment)
	{
		if (!segment.IsolationRoot
			|| segment.Layer != PresentationSceneContentLayer
			|| !segment.RasterizesAncestorGeometryClip
			|| segment.AncestorGeometryClips.empty()
			|| segment.IsolationRoots.size() != 1u
			|| segment.IsolationRoots.front().Get()
				!= segment.IsolationRoot.Get()
			|| !segment.OpacityIsolationRoots.empty()
			|| !segment.OpacityGroupRoots.empty()
			|| segment.NodeStart >= _nodes.size()
			|| segment.NodeEnd <= segment.NodeStart
			|| segment.NodeEnd > _nodes.size()) return false;
		auto* root = segment.IsolationRoot.Get();
		return root
			&& _nodes[segment.NodeStart].Element.Get() == root
			&& _nodes[segment.NodeStart].SubtreeEnd == segment.NodeEnd
			&& !_nodes[segment.NodeStart].NativeComposition
			&& !_nodes[segment.NodeStart].Overlay;
	};
	std::vector<Segment> compactedSegments;
	compactedSegments.reserve(_segments.size());
	for (auto& source : _segments)
	{
		bool merge = false;
		if (!compactedSegments.empty()
			&& standaloneGeometrySibling(source))
		{
			auto& previous = compactedSegments.back();
			auto* previousRoot = previous.GeometryRasterMembers.empty()
				? previous.IsolationRoot.Get()
				: previous.GeometryRasterMembers.front().IsolationRoot.Get();
			auto* sourceRoot = source.IsolationRoot.Get();
			const bool previousEligible =
				!previous.GeometryRasterMembers.empty()
				|| standaloneGeometrySibling(previous);
			merge = previousEligible
				&& previous.NodeEnd == source.NodeStart
				&& previousRoot && sourceRoot
				&& previousRoot->GetVisualParent()
					== sourceRoot->GetVisualParent()
				&& previous.HasAncestorClip == source.HasAncestorClip
				&& exactClipBounds(
					previous.AncestorClipRootDip,
					source.AncestorClipRootDip)
				&& exactGeometryChain(previous, source);
		}
		if (!merge)
		{
			const size_t compactedIndex = compactedSegments.size();
			for (size_t nodeIndex = source.NodeStart;
				nodeIndex < source.NodeEnd && nodeIndex < _nodes.size();
				++nodeIndex)
			{
				_nodes[nodeIndex].SegmentIndex = compactedIndex;
				_nodes[nodeIndex].GeometryRasterMemberIndex = NoSegment;
			}
			compactedSegments.push_back(std::move(source));
			continue;
		}

		auto& destination = compactedSegments.back();
		if (destination.GeometryRasterMembers.empty())
		{
			Segment::GeometryRasterMember first;
			first.IsolationRoot = destination.IsolationRoot;
			first.IsolationRoots = destination.IsolationRoots;
			first.NodeStart = destination.NodeStart;
			first.NodeEnd = destination.NodeEnd;
			destination.GeometryRasterMembers.push_back(std::move(first));
			for (size_t nodeIndex = destination.NodeStart;
				nodeIndex < destination.NodeEnd && nodeIndex < _nodes.size();
				++nodeIndex)
				_nodes[nodeIndex].GeometryRasterMemberIndex = 0u;
		}
		Segment::GeometryRasterMember member;
		member.IsolationRoot = source.IsolationRoot;
		member.IsolationRoots = std::move(source.IsolationRoots);
		member.NodeStart = source.NodeStart;
		member.NodeEnd = source.NodeEnd;
		const size_t memberIndex = destination.GeometryRasterMembers.size();
		destination.GeometryRasterMembers.push_back(std::move(member));
		destination.NodeEnd = source.NodeEnd;
		const size_t compactedIndex = compactedSegments.size() - 1u;
		for (size_t nodeIndex = source.NodeStart;
			nodeIndex < source.NodeEnd && nodeIndex < _nodes.size(); ++nodeIndex)
		{
			_nodes[nodeIndex].SegmentIndex = compactedIndex;
			_nodes[nodeIndex].GeometryRasterMemberIndex = memberIndex;
		}
	}
	_segments = std::move(compactedSegments);
	for (const auto candidate : candidates)
	{
		if (!sharedOpacityGroupRoot[candidate]) continue;
		auto* root = _nodes[candidate].Element.Get();
		size_t first = NoSegment;
		size_t last = NoSegment;
		for (size_t segmentIndex = 0;
			segmentIndex < _segments.size(); ++segmentIndex)
		{
			const auto& segment = _segments[segmentIndex];
			const bool member = std::any_of(
				segment.OpacityGroupRoots.begin(),
				segment.OpacityGroupRoots.end(),
				[root](const ControlWeakReference& value)
				{ return value.Get() == root; });
			if (!member) continue;
			if (first == NoSegment) first = segmentIndex;
			last = segmentIndex;
		}
		if (first == NoSegment || last < first) continue;
		bool contiguous = true;
		for (size_t segmentIndex = first; segmentIndex <= last; ++segmentIndex)
		{
			const auto& roots = _segments[segmentIndex].OpacityGroupRoots;
			contiguous = contiguous && std::any_of(
				roots.begin(), roots.end(),
				[root](const ControlWeakReference& value)
				{ return value.Get() == root; });
		}
		if (!contiguous) continue;
		_opacityGroups.push_back(OpacityGroup{
			ControlWeakReference(root), first, last - first + 1u,
			NoSegment, _segments[first].Order, 1.0f });
		_opacityGroups.back().Layer = _segments[first].Layer;
	}
	for (size_t groupIndex = 0;
		groupIndex < _opacityGroups.size(); ++groupIndex)
	{
		auto& group = _opacityGroups[groupIndex];
		const size_t groupEnd = group.FirstSegment + group.SegmentCount;
		for (size_t reverseParent = groupIndex;
			reverseParent > 0u; --reverseParent)
		{
			const size_t parentIndex = reverseParent - 1u;
			const auto& parent = _opacityGroups[parentIndex];
			const size_t parentEnd =
				parent.FirstSegment + parent.SegmentCount;
			if (group.FirstSegment >= parent.FirstSegment
				&& groupEnd <= parentEnd
				&& (group.FirstSegment != parent.FirstSegment
					|| groupEnd != parentEnd))
			{
				group.ParentGroup = parentIndex;
				break;
			}
		}
	}
	for (const auto& node : _nodes)
	{
		if (!node.NativeComposition) continue;
		auto* nativeRoot = node.Element.Get();
		if (!nativeRoot) continue;
		size_t owner = NoSegment;
		for (size_t groupIndex = 0;
			groupIndex < _opacityGroups.size(); ++groupIndex)
		{
			auto* groupRoot = _opacityGroups[groupIndex].Root.Get();
			const auto found = groupRoot ? _nodeIndex.find(groupRoot)
				: _nodeIndex.end();
			if (found == _nodeIndex.end() || found->second >= _nodes.size())
				continue;
			const size_t groupEnd = (std::min)(
				_nodes[found->second].SubtreeEnd, _nodes.size());
			const auto nativeFound = _nodeIndex.find(nativeRoot);
			if (nativeFound != _nodeIndex.end()
				&& nativeFound->second > found->second
				&& nativeFound->second < groupEnd) owner = groupIndex;
		}
		if (owner != NoSegment)
			_opacityGroups[owner].NativeMembers.push_back({
				ControlWeakReference(nativeRoot), node.Order });
	}
}

void PresentationScene::BeginFrameStatistics() noexcept
{
	_frameStatistics = {};
	AdvanceRevision(_frameSequence);
	_frameStatistics.Frame = _frameSequence;
	_frameStatistics.CommandCacheInvalidatedNodes =
		_pendingCommandCacheInvalidations;
	_pendingCommandCacheInvalidations = 0;
}

bool PresentationScene::RefreshNodeState(Node& node)
{
	auto* control = node.Element.Get();
	if (!control) return false;
	const auto revisions = control->GetPresentationRevisions();
	if (revisions.Content != node.AppliedRevisions.Content)
		node.ContentDirty = true;
	if (revisions.Geometry != node.AppliedRevisions.Geometry)
		node.GeometryDirty = true;
	if (revisions.Composition != node.AppliedRevisions.Composition)
		node.CompositionDirty = true;
	return node.Element.Get() == control;
}

void PresentationScene::ApplyPendingGeometryInvalidations()
{
	if (_allGeometryDirty)
	{
		for (auto& node : _nodes)
		{
			node.GeometryDirty = true;
			node.TransformOnlyGeometryDirty = false;
		}
		_allGeometryDirty = false;
		_pendingGeometryRanges.clear();
		return;
	}
	if (_pendingGeometryRanges.empty()) return;
	std::sort(_pendingGeometryRanges.begin(), _pendingGeometryRanges.end(),
		[](const PendingGeometryRange& left,
			const PendingGeometryRange& right)
		{
			if (left.Start != right.Start) return left.Start < right.Start;
			if (left.End != right.End) return left.End < right.End;
			return left.TransformOnly < right.TransformOnly;
		});
	size_t rangeStart = _pendingGeometryRanges.front().Start;
	size_t rangeEnd = _pendingGeometryRanges.front().End;
	bool transformOnly = _pendingGeometryRanges.front().TransformOnly;
	auto applyRange = [this](
		size_t start, size_t end, bool onlyTransform)
	{
		end = (std::min)(end, _nodes.size());
		for (size_t index = (std::min)(start, end); index < end; ++index)
		{
			auto& node = _nodes[index];
			if (!onlyTransform)
				node.TransformOnlyGeometryDirty = false;
			else if (!node.GeometryDirty)
				node.TransformOnlyGeometryDirty = true;
			node.GeometryDirty = true;
		}
	};
	for (size_t index = 1; index < _pendingGeometryRanges.size(); ++index)
	{
		const auto& next = _pendingGeometryRanges[index];
		const auto nextStart = next.Start;
		const auto nextEnd = next.End;
		if (nextStart <= rangeEnd)
		{
			rangeEnd = (std::max)(rangeEnd, nextEnd);
			transformOnly = transformOnly && next.TransformOnly;
			continue;
		}
		applyRange(rangeStart, rangeEnd, transformOnly);
		rangeStart = nextStart;
		rangeEnd = nextEnd;
		transformOnly = next.TransformOnly;
	}
	applyRange(rangeStart, rangeEnd, transformOnly);
	_pendingGeometryRanges.clear();
}

bool PresentationScene::RefreshNodeGeometry(Node& node)
{
	auto* control = node.Element.Get();
	if (!control) return false;
	if (!node.GeometryDirty && node.HasGeometry) return true;
	const auto geometryRevision =
		control->GetPresentationRevisions().Geometry;
	node.RenderedBounds = control->GetRenderedAbsoluteRectDip();
	control = node.Element.Get();
	if (!control) return false;
	node.HasGeometry = true;
	node.AppliedRevisions.Geometry = geometryRevision;
	node.GeometryDirty =
		control->GetPresentationRevisions().Geometry != geometryRevision;
	if (!node.GeometryDirty) node.TransformOnlyGeometryDirty = false;
	++_frameStatistics.GeometryRecomputedNodes;
	return true;
}

bool PresentationScene::CompleteNode(
	Node& node,
	const PresentationRevisionSnapshot& submitted)
{
	auto* control = node.Element.Get();
	if (!control) return false;
	node.AppliedRevisions = submitted;
	const auto current = control->GetPresentationRevisions();
	node.ContentDirty = current.Content != submitted.Content;
	node.GeometryDirty = current.Geometry != submitted.Geometry;
	node.TransformOnlyGeometryDirty = false;
	node.CompositionDirty = current.Composition != submitted.Composition;
	node.HasPresented = true;
	return true;
}

bool PresentationScene::PrepareNodeForRendering(
	Node& node,
	Control*& control)
{
	control = node.Element.Get();
	if (!control) return false;
	control->PreparePresentation();
	control = node.Element.Get();
	return control && RefreshNodeState(node);
}

bool PresentationScene::PrepareComposition(
	PresentationRenderHost& host,
	int titleBarOffsetDip,
	float dpiScale)
{
	if (!host.UsesComposition()) return false;
	PresentationPreparationStatistics preparation;
	// Preserve the invalidation lane before RefreshNodeState observes the same
	// revision delta. In particular, a transform-only range must be classified
	// while the retained node is still clean.
	ApplyPendingGeometryInvalidations();
	try
	{
		SceneWorkAccumulator scratchClock(preparation.ScratchMicroseconds);
		_preparedNodeScratch.assign(_nodes.size(), uint8_t{ 0 });
	}
	catch (...)
	{
		return false;
	}
	// Publish pending layout before deriving either retained surface geometry or
	// visual-only opacity. RenderComposition reuses these preparation marks so a
	// stable isolated frame does not invoke PreparePresentation twice.
	{
		SceneWorkAccumulator nodeClock(
			preparation.NodePreparationMicroseconds);
		std::vector<size_t> ancestorScratch;
		try { ancestorScratch.reserve(16u); }
		catch (...) { return false; }
		auto prepareNode = [&](size_t nodeIndex)
		{
			if (nodeIndex >= _nodes.size()) return false;
			if (_preparedNodeScratch[nodeIndex] != 0u) return true;
			Control* prepared = nullptr;
			if (!PrepareNodeForRendering(_nodes[nodeIndex], prepared))
				return false;
			_preparedNodeScratch[nodeIndex] = uint8_t{ 1 };
			++preparation.PreparedNodeCount;
			return true;
		};
		for (const auto& segment : _segments)
		{
			auto* isolationRoot = segment.IsolationRoot.Get();
			if (!isolationRoot) continue;
			// Attached layout animations can invalidate an ancestor's arrange policy.
			// Commit that chain root-to-leaf before reading isolation bounds; the
			// preparation marks keep shared ancestors single-pass.
			ancestorScratch.clear();
			try
			{
				for (auto* ancestor = isolationRoot->GetVisualParent();
					ancestor; ancestor = ancestor->GetVisualParent())
				{
					const auto found = _nodeIndex.find(ancestor);
					if (found != _nodeIndex.end()
						&& found->second < _nodes.size()
						&& _preparedNodeScratch[found->second] == 0u)
						ancestorScratch.push_back(found->second);
					if (cui::framework::PresentationAccess::
						BreaksVisualPresentationInheritance(*ancestor)) break;
				}
			}
			catch (...) { return false; }
			for (auto it = ancestorScratch.rbegin();
				it != ancestorScratch.rend(); ++it)
				if (!prepareNode(*it)) return false;
			const size_t end = (std::min)(segment.NodeEnd, _nodes.size());
			for (size_t nodeIndex = (std::min)(segment.NodeStart, end);
				nodeIndex < end; ++nodeIndex)
			{
				auto& node = _nodes[nodeIndex];
				if (node.NativeComposition
					|| (node.Overlay && !node.OverlaySegmented)
					|| !prepareNode(nodeIndex)) return false;
			}
		}
	}
	ApplyPendingGeometryInvalidations();
	if (_structureDirty) return false;
	struct SceneLayerTopologyBatchScope final
	{
		PresentationRenderHost* Host = nullptr;
		bool Active = false;
		~SceneLayerTopologyBatchScope()
		{
			if (Active && Host) Host->RollbackSceneLayerTopologyBatch();
		}
	} topologyBatch{ &host, false };
	if (!host.EnsureSceneLayerSlots(_segments.size())) return false;
	// ID2D1Geometry is a device-independent factory resource. Keep one native
	// materialization for every distinct authored geometry/transform pair and
	// bind that same immutable mask to all retained surfaces that share it.
	// This preserves exact D2D layer coverage while eliminating repeated path
	// parsing and transformed-geometry creation across sibling scene layers.
	SceneWorkClock segmentsClock;
	for (size_t index = 0; index < _segments.size(); ++index)
	{
		++preparation.SegmentCount;
		auto& segment = _segments[index];
		if (segment.GeometryRasterMembers.size() > 1u)
		{
			++preparation.GeometryRasterGroupCount;
			preparation.GeometryRasterMemberCount +=
				segment.GeometryRasterMembers.size();
		}
		PresentationRenderHost::SceneLayerVisualProperties properties;
		bool hasRenderableBounds = !segment.IsolationRoot;
		segment.SurfaceOriginDip = {};
		segment.LogicalSurfaceClient = RECT{
			0, 0,
			static_cast<LONG>((std::max)(UINT{ 1 }, host.PhysicalWidth())),
			static_cast<LONG>((std::max)(UINT{ 1 }, host.PhysicalHeight())) };
		segment.SurfaceProperties = {
			(std::max)(UINT{ 1 }, host.PhysicalWidth()),
			(std::max)(UINT{ 1 }, host.PhysicalHeight()), true };
		segment.LogicalTransform = D2D1::Matrix3x2F::Identity();
		segment.LogicalInverse = D2D1::Matrix3x2F::Identity();
		segment.HasLogicalInverse = true;
		segment.StagedOpacity = 1.0f;
		if (auto* isolationRoot = segment.IsolationRoot.Get())
		{
			SceneWorkClock rootStateClock;
			if (!std::isfinite(dpiScale) || dpiScale <= 0.0f)
				return false;
			// An unmaterialized one-node content isolation whose live render AABB
			// is wholly outside the viewport cannot contribute pixels. Rebuild has
			// already classified arbitrary ancestor geometry clips, and clip changes
			// invalidate structure, so this path can retain the logical slot without
			// allocating transient clip/bounds vectors or staging physical properties.
			const bool earlyViewportEligible =
				!host.IsSceneLayerMaterialized(index)
				&& segment.Layer == PresentationSceneContentLayer
				&& segment.NodeEnd == segment.NodeStart + 1u
				&& segment.IsolationRoots.size() == 1u
				&& segment.IsolationRoots.front().Get() == isolationRoot
				&& segment.OpacityIsolationRoots.empty()
				&& segment.OpacityGroupRoots.empty()
				&& !segment.RasterizesAncestorGeometryClip;
			if (earlyViewportEligible)
			{
				const auto liveBounds = isolationRoot->GetRenderedAbsoluteRectDip();
				constexpr double SurfacePaddingDip = 2.0;
				const double physicalLeft = std::floor(
					(static_cast<double>(liveBounds.left) - SurfacePaddingDip)
					* dpiScale);
				const double physicalTop = std::floor(
					(static_cast<double>(liveBounds.top)
						+ static_cast<double>(titleBarOffsetDip)
						- SurfacePaddingDip) * dpiScale);
				const double physicalRight = std::ceil(
					(static_cast<double>(liveBounds.right) + SurfacePaddingDip)
					* dpiScale);
				const double physicalBottom = std::ceil(
					(static_cast<double>(liveBounds.bottom)
						+ static_cast<double>(titleBarOffsetDip)
						+ SurfacePaddingDip) * dpiScale);
				const bool finiteArea = std::isfinite(physicalLeft)
					&& std::isfinite(physicalTop)
					&& std::isfinite(physicalRight)
					&& std::isfinite(physicalBottom)
					&& physicalRight > physicalLeft
					&& physicalBottom > physicalTop;
				const bool whollyOutsideViewport = finiteArea
					&& (physicalRight <= 0.0
						|| physicalLeft >= static_cast<double>(host.PhysicalWidth())
						|| physicalBottom <= 0.0
						|| physicalTop >= static_cast<double>(host.PhysicalHeight()));
				if (whollyOutsideViewport)
				{
					preparation.RootStateMicroseconds +=
						rootStateClock.ElapsedMicroseconds();
					segment.Context = nullptr;
					++preparation.DeferredUnmaterializedCount;
					++preparation.EarlyViewportDeferredCount;
					continue;
				}
			}
			Control* singleIsolationRoot = nullptr;
			std::vector<Control*> liveIsolationRoots;
			if (segment.IsolationRoots.size() == 1u)
			{
				singleIsolationRoot = segment.IsolationRoots.front().Get();
				if (!singleIsolationRoot) return false;
			}
			else
			{
				liveIsolationRoots.reserve(segment.IsolationRoots.size());
				for (const auto& weakRoot : segment.IsolationRoots)
				{
					auto* liveRoot = weakRoot.Get();
					if (!liveRoot) return false;
					liveIsolationRoots.push_back(liveRoot);
				}
			}
			if ((singleIsolationRoot && singleIsolationRoot != isolationRoot)
				|| (!liveIsolationRoots.empty()
					&& liveIsolationRoots.back() != isolationRoot)
				|| (!singleIsolationRoot && liveIsolationRoots.empty()))
				return false;
			double opacity = 1.0;
			for (const auto& weakRoot : segment.OpacityIsolationRoots)
			{
				auto* liveRoot = weakRoot.Get();
				if (!liveRoot) return false;
				const double value = liveRoot->GetOpacity();
				if (!std::isfinite(value)
					|| value < 0.0 || value > 1.0) return false;
				opacity *= value;
			}
			segment.StagedOpacity = static_cast<float>(opacity);
			properties.Opacity = segment.StagedOpacity;
			const auto suppressionRoots = singleIsolationRoot
				? std::span<Control* const>{ &singleIsolationRoot, 1u }
				: std::span<Control* const>{
					liveIsolationRoots.data(), liveIsolationRoots.size() };
			preparation.RootStateMicroseconds +=
				rootStateClock.ElapsedMicroseconds();
			SceneWorkClock ancestorClipClock;
			std::vector<AncestorRectangleClip> liveAncestorClipChain;
			std::vector<AncestorGeometryClip> liveAncestorGeometryClips;
			bool canFlattenAncestorClip = true;
			bool liveHasAncestorClip = false;
			D2D1_RECT_F liveAncestorClip{};
			Control* sharedOpacityBoundary = nullptr;
			if (!segment.OpacityGroupRoots.empty())
			{
				// The innermost shared parent owns the nearest common clip;
				// its ancestors are already represented by outer group visuals.
				sharedOpacityBoundary = segment.OpacityGroupRoots.back().Get();
				if (!sharedOpacityBoundary) return false;
			}
			if (!TryResolveAncestorClips(
				isolationRoot, liveAncestorClipChain,
				liveAncestorGeometryClips,
				canFlattenAncestorClip,
				liveHasAncestorClip, liveAncestorClip,
				sharedOpacityBoundary))
				return false;
			segment.HasAncestorClip = liveHasAncestorClip;
			segment.AncestorClipRootDip = liveAncestorClip;
			segment.AncestorClipChain.clear();
			segment.AncestorClipChain.reserve(liveAncestorClipChain.size());
			for (const auto& clip : liveAncestorClipChain)
				segment.AncestorClipChain.push_back({
					clip.LocalRect, clip.RadiusX, clip.RadiusY,
					clip.LocalToRoot });
			segment.AncestorGeometryClips.resize(
				liveAncestorGeometryClips.size());
			for (size_t clipIndex = 0;
				clipIndex < liveAncestorGeometryClips.size(); ++clipIndex)
			{
				const auto& clip = liveAncestorGeometryClips[clipIndex];
				const D2D1_MATRIX_3X2_F nativeTransform =
					AsMatrix(clip.LocalToRoot)
					* D2D1::Matrix3x2F::Translation(
						0.0f, static_cast<float>(titleBarOffsetDip));
				Microsoft::WRL::ComPtr<ID2D1Geometry> native;
				auto reuse = [&](const Segment::AncestorGeometryClip& value)
				{
					return value.NativeGeometry
						&& value.Value == clip.Value
						&& SameMatrix(value.NativeTransformDip, nativeTransform);
				};
				auto& accepted = segment.AncestorGeometryClips[clipIndex];
				if (reuse(accepted)) native = accepted.NativeGeometry;
				for (size_t sharedIndex = 0;
					!native && sharedIndex < clipIndex; ++sharedIndex)
				{
					const auto& shared =
						segment.AncestorGeometryClips[sharedIndex];
					if (reuse(shared)) native = shared.NativeGeometry;
				}
				for (size_t segmentIndex = 0;
					!native && segmentIndex < index; ++segmentIndex)
				{
					const auto& sharedMasks =
						_segments[segmentIndex].AncestorGeometryClips;
					const auto shared = std::find_if(
						sharedMasks.begin(), sharedMasks.end(), reuse);
					if (shared != sharedMasks.end())
						native = shared->NativeGeometry;
				}
				if (native)
					++preparation.AncestorGeometryMaskReuseCount;
				else
				{
					native.Attach(clip.Value.CreateD2DGeometry(&nativeTransform));
					if (!native) return false;
					++preparation.AncestorGeometryMaskMaterializationCount;
				}
				accepted.Value = clip.Value;
				accepted.LocalToRootDip = clip.LocalToRoot;
				accepted.NativeTransformDip = nativeTransform;
				accepted.NativeGeometry = std::move(native);
			}
			segment.RasterizesAncestorGeometryClip =
				!segment.AncestorGeometryClips.empty();
			preparation.AncestorClipMicroseconds +=
				ancestorClipClock.ElapsedMicroseconds();
			SceneWorkClock boundsClock;
			D2D1_RECT_F baseBounds{};
			bool hasBaseBounds = false;
			{
				cui::framework::PresentationAccess::
					RenderTransformSuppressionScope suppress(suppressionRoots);
				const auto end = (std::min)(segment.NodeEnd, _nodes.size());
				for (size_t nodeIndex = (std::min)(segment.NodeStart, end);
					nodeIndex < end; ++nodeIndex)
				{
					auto* control = _nodes[nodeIndex].Element.Get();
					if (!control || !control->IsVisible) continue;
					const auto bounds = cui::framework::PresentationAccess::
						RenderedAbsoluteRectForRecording(*control);
					if (!std::isfinite(bounds.left)
						|| !std::isfinite(bounds.top)
						|| !std::isfinite(bounds.right)
						|| !std::isfinite(bounds.bottom)) return false;
					if (bounds.right <= bounds.left
						|| bounds.bottom <= bounds.top) continue;
					if (!hasBaseBounds)
					{
						baseBounds = bounds;
						hasBaseBounds = true;
					}
					else
					{
						baseBounds.left = (std::min)(
							baseBounds.left, bounds.left);
						baseBounds.top = (std::min)(
							baseBounds.top, bounds.top);
						baseBounds.right = (std::max)(
							baseBounds.right, bounds.right);
						baseBounds.bottom = (std::max)(
							baseBounds.bottom, bounds.bottom);
					}
				}
			}
			hasRenderableBounds = hasBaseBounds;
			if (!hasBaseBounds)
			{
				// A transient/group container can own only descendants and have an
				// empty authored render box. It still needs a contiguous graph slot,
				// but must not make the complete atomic overlay segmentation fail.
				// A padded one-DIP transparent surface preserves that slot; the normal
				// node culling path submits no pixels for the empty control itself.
				const auto emptyBounds = cui::framework::PresentationAccess::
					RenderedAbsoluteRectForRecording(*isolationRoot);
				if (!std::isfinite(emptyBounds.left)
					|| !std::isfinite(emptyBounds.top)) return false;
				baseBounds = D2D1::RectF(
					emptyBounds.left, emptyBounds.top,
					emptyBounds.left + 1.0f, emptyBounds.top + 1.0f);
				hasBaseBounds = true;
			}
			if (segment.RasterizesAncestorGeometryClip
				&& liveHasAncestorClip)
				baseBounds = liveAncestorClip;
			// Control::BeginRender records the Window title-bar translation into
			// every command list. Surface bounds therefore live in the same client
			// coordinate space; omitting this offset produces a correctly sized but
			// fully transparent isolated swap chain.
			baseBounds.top += static_cast<float>(titleBarOffsetDip);
			baseBounds.bottom += static_cast<float>(titleBarOffsetDip);

			// Match retained damage inflation and align the backing surface to
			// physical pixels. The DComp visual transform later restores this
			// origin before applying the live global transform.
			constexpr double SurfacePaddingDip = 2.0;
			const double physicalLeft = std::floor(
				(static_cast<double>(baseBounds.left) - SurfacePaddingDip)
				* dpiScale);
			const double physicalTop = std::floor(
				(static_cast<double>(baseBounds.top) - SurfacePaddingDip)
				* dpiScale);
			const double physicalRight = std::ceil(
				(static_cast<double>(baseBounds.right) + SurfacePaddingDip)
				* dpiScale);
			const double physicalBottom = std::ceil(
				(static_cast<double>(baseBounds.bottom) + SurfacePaddingDip)
				* dpiScale);
			const double physicalWidth = physicalRight - physicalLeft;
			const double physicalHeight = physicalBottom - physicalTop;
			if (!std::isfinite(physicalLeft) || !std::isfinite(physicalTop)
				|| !std::isfinite(physicalWidth) || !std::isfinite(physicalHeight)
				|| physicalWidth < 1.0 || physicalHeight < 1.0
				|| physicalWidth > (std::numeric_limits<UINT>::max)()
				|| physicalHeight > (std::numeric_limits<UINT>::max)()
				|| std::abs(physicalLeft) > (std::numeric_limits<float>::max)()
				|| std::abs(physicalTop) > (std::numeric_limits<float>::max)())
				return false;
			segment.SurfaceOriginDip = D2D1::Point2F(
				static_cast<float>(physicalLeft / dpiScale),
				static_cast<float>(physicalTop / dpiScale));
			segment.SurfaceProperties = {
				static_cast<UINT>(physicalWidth),
				static_cast<UINT>(physicalHeight), false };
			const double logicalSurfaceWidth = std::ceil(
				physicalWidth / dpiScale);
			const double logicalSurfaceHeight = std::ceil(
				physicalHeight / dpiScale);
			if (logicalSurfaceWidth > (std::numeric_limits<LONG>::max)()
				|| logicalSurfaceHeight > (std::numeric_limits<LONG>::max)())
				return false;
			segment.LogicalSurfaceClient = RECT{
				0, 0, static_cast<LONG>(logicalSurfaceWidth),
				static_cast<LONG>(logicalSurfaceHeight) };
			preparation.BoundsMicroseconds +=
				boundsClock.ElapsedMicroseconds();

			SceneWorkClock transformClock;
			const auto title = D2D1::Matrix3x2F::Translation(
				0.0f, static_cast<float>(titleBarOffsetDip));
			const auto normal = AsMatrix(
				isolationRoot->GetLocalToRenderTransform()) * title;
			D2D1::Matrix3x2F base;
			{
				cui::framework::PresentationAccess::
					RenderTransformSuppressionScope suppress(suppressionRoots);
				base = AsMatrix(
					cui::framework::PresentationAccess::
						LocalToRenderTransformForRecording(*isolationRoot)) * title;
			}
			auto inverseBase = base;
			if (!inverseBase.Invert()) return false;
			const auto logical = inverseBase * normal;
			auto logicalInverse = logical;
			segment.LogicalTransform = logical;
			segment.HasLogicalInverse = logicalInverse.Invert();
			segment.LogicalInverse = segment.HasLogicalInverse
				? logicalInverse : D2D1::Matrix3x2F::Identity();
			if (!segment.GeometryRasterMembers.empty())
			{
				auto& first = segment.GeometryRasterMembers.front();
				if (first.IsolationRoot.Get() != isolationRoot
					|| first.IsolationRoots.size() != 1u
					|| first.IsolationRoots.front().Get() != isolationRoot)
					return false;
				first.LogicalTransform = segment.LogicalTransform;
				first.LogicalInverse = segment.LogicalInverse;
				first.HasLogicalInverse = segment.HasLogicalInverse;
				for (size_t memberIndex = 1u;
					memberIndex < segment.GeometryRasterMembers.size();
					++memberIndex)
				{
					auto& member =
						segment.GeometryRasterMembers[memberIndex];
					auto* memberRoot = member.IsolationRoot.Get();
					if (!memberRoot || member.IsolationRoots.size() != 1u
						|| member.IsolationRoots.front().Get() != memberRoot)
						return false;
					const auto memberNormal = AsMatrix(
						memberRoot->GetLocalToRenderTransform()) * title;
					D2D1::Matrix3x2F memberBase;
					{
						cui::framework::PresentationAccess::
							RenderTransformSuppressionScope suppress(
								std::span<Control* const>{ &memberRoot, 1u });
						memberBase = AsMatrix(
							cui::framework::PresentationAccess::
								LocalToRenderTransformForRecording(*memberRoot))
							* title;
					}
					auto memberInverseBase = memberBase;
					if (!memberInverseBase.Invert()) return false;
					const auto memberLogical =
						memberInverseBase * memberNormal;
					auto memberLogicalInverse = memberLogical;
					member.LogicalTransform = memberLogical;
					member.HasLogicalInverse = memberLogicalInverse.Invert();
					member.LogicalInverse = member.HasLogicalInverse
						? memberLogicalInverse
						: D2D1::Matrix3x2F::Identity();
				}
			}
			const auto publishedLogical =
				segment.RasterizesAncestorGeometryClip
				? D2D1::Matrix3x2F::Identity() : logical;
			properties.PhysicalTransform =
				D2D1::Matrix3x2F::Translation(
					static_cast<float>(physicalLeft),
					static_cast<float>(physicalTop))
				* cui::dcomp_detail::DipTransformToPhysicalPixels(
					publishedLogical, dpiScale);
			if (segment.HasAncestorClip && canFlattenAncestorClip)
			{
				const auto& clip = segment.AncestorClipRootDip;
				properties.PhysicalClip = D2D1::RectF(
					clip.left * dpiScale,
					(clip.top + static_cast<float>(titleBarOffsetDip))
						* dpiScale,
					clip.right * dpiScale,
					(clip.bottom + static_cast<float>(titleBarOffsetDip))
						* dpiScale);
				properties.HasClip = true;
			}
			else if (segment.HasAncestorClip)
			{
				properties.TransformedClipChain.reserve(
					segment.AncestorClipChain.size());
				for (const auto& clip : segment.AncestorClipChain)
				{
					auto physicalTransform =
						cui::dcomp_detail::DipTransformToPhysicalPixels(
							clip.LocalToRootDip, dpiScale,
							static_cast<float>(titleBarOffsetDip) * dpiScale);
					properties.TransformedClipChain.push_back({
						D2D1::RectF(
							clip.LocalRectDip.left * dpiScale,
							clip.LocalRectDip.top * dpiScale,
							clip.LocalRectDip.right * dpiScale,
							clip.LocalRectDip.bottom * dpiScale),
						clip.RadiusXDip * dpiScale,
						clip.RadiusYDip * dpiScale,
						physicalTransform });
				}
			}
			preparation.TransformClassificationMicroseconds +=
				transformClock.ElapsedMicroseconds();
		}
		SceneWorkClock classificationClock;
		const bool materialized = host.IsSceneLayerMaterialized(index);
		const auto projectedSurface = TransformBounds(
			D2D1::RectF(
				0.0f, 0.0f,
				static_cast<float>(segment.SurfaceProperties.PhysicalWidth),
				static_cast<float>(segment.SurfaceProperties.PhysicalHeight)),
			properties.PhysicalTransform);
		const bool intersectsViewport =
			std::isfinite(projectedSurface.left)
			&& std::isfinite(projectedSurface.top)
			&& std::isfinite(projectedSurface.right)
			&& std::isfinite(projectedSurface.bottom)
			&& projectedSurface.left < static_cast<float>(host.PhysicalWidth())
			&& projectedSurface.right > 0.0f
			&& projectedSurface.top < static_cast<float>(host.PhysicalHeight())
			&& projectedSurface.bottom > 0.0f;
		// Lazy materialization is deliberately restricted to the proven simple
		// content-isolation shape. Shared opacity parents, nested isolation,
		// transient overlay roots and arbitrary geometry masks own graph/lease
		// semantics even when their D2D pixels are currently outside the viewport.
		const bool lazyMaterializationEligible = segment.IsolationRoot
			&& segment.Layer == PresentationSceneContentLayer
			&& segment.IsolationRoots.size() == 1u
			&& segment.OpacityIsolationRoots.empty()
			&& segment.OpacityGroupRoots.empty()
			&& !segment.RasterizesAncestorGeometryClip;
		const bool requiresPhysicalLayer = materialized
			|| !lazyMaterializationEligible
			|| (hasRenderableBounds && intersectsViewport);
		preparation.TransformClassificationMicroseconds +=
			classificationClock.ElapsedMicroseconds();
		if (!requiresPhysicalLayer)
		{
			segment.Context = nullptr;
			++preparation.DeferredUnmaterializedCount;
			continue;
		}
		++preparation.PhysicalLayerRequiredCount;
		SceneWorkClock layerClock;
		if (!materialized && !topologyBatch.Active)
		{
			if (!host.BeginSceneLayerTopologyBatch()) return false;
			topologyBatch.Active = true;
		}
		segment.Context = host.AcquireSceneLayer(
			index, segment.Layer, segment.Order,
			segment.SurfaceProperties);
		if (!segment.Context
			|| !host.StageSceneLayerVisualProperties(index, properties))
		{
			for (auto& item : _segments) item.Context = nullptr;
			return false;
		}
		preparation.LayerAcquireStageMicroseconds +=
			layerClock.ElapsedMicroseconds();
	}
	preparation.SegmentMicroseconds += segmentsClock.ElapsedMicroseconds();
	if (topologyBatch.Active)
	{
		SceneWorkClock topologyClock;
		const bool committed = host.CommitSceneLayerTopologyBatch();
		preparation.TopologyCommitMicroseconds +=
			topologyClock.ElapsedMicroseconds();
		topologyBatch.Active = false;
		if (!committed)
		{
			for (auto& item : _segments) item.Context = nullptr;
			return false;
		}
	}
	SceneWorkClock groupClock;
	host.TrimSceneLayers(_segments.size());
	try
	{
		_groupPropertiesScratch.resize(_opacityGroups.size());
		for (size_t groupIndex = 0;
			groupIndex < _opacityGroups.size(); ++groupIndex)
		{
			auto& group = _opacityGroups[groupIndex];
			auto* root = group.Root.Get();
			if (!root) return false;
			const double opacity = root->GetOpacity();
			if (!std::isfinite(opacity)
				|| opacity < 0.0 || opacity > 1.0) return false;
			group.StagedOpacity = static_cast<float>(opacity);
			auto& properties = _groupPropertiesScratch[groupIndex];
			properties.FirstLayer = group.FirstSegment;
			properties.LayerCount = group.SegmentCount;
			properties.ParentGroup = group.ParentGroup == NoSegment
				? PresentationRenderHost::SceneLayerGroupProperties::NoParent
				: group.ParentGroup;
			properties.Opacity = group.StagedOpacity;
			properties.Layer = group.Layer;
			properties.Order = group.Order;
			properties.PhysicalClip = {};
			properties.HasClip = false;
			properties.TransformedClipChain.clear();
			properties.NativeVisuals.clear();
			properties.NativeVisuals.reserve(group.NativeMembers.size());
			for (const auto& native : group.NativeMembers)
			{
				auto* control = native.Root.Get();
				if (!control) return false;
				control->PreparePresentation();
				if (!control->PrepareNativeCompositionVisual()) return false;
				auto* visual = control->GetNativeCompositionVisual();
				if (!visual) return false;
				properties.NativeVisuals.push_back({ visual, native.Order });
			}

			std::vector<AncestorRectangleClip> groupClipChain;
			std::vector<AncestorGeometryClip> groupGeometryClips;
			bool canFlattenGroupClip = true;
			bool hasGroupClip = false;
			D2D1_RECT_F groupClipRootDip{};
			Control* parentBoundary = nullptr;
			if (group.ParentGroup != NoSegment)
			{
				if (group.ParentGroup >= _opacityGroups.size()) return false;
				parentBoundary = _opacityGroups[group.ParentGroup].Root.Get();
				if (!parentBoundary) return false;
			}
			if (!TryResolveAncestorClips(root, groupClipChain,
				groupGeometryClips, canFlattenGroupClip,
				hasGroupClip, groupClipRootDip, parentBoundary, true)) return false;
			if (!groupGeometryClips.empty())
			{
				// A shared DComp parent has no arbitrary Geometry mask input.
				// Reclassify on the next synchronization rather than duplicating
				// this coverage mask independently on each child surface.
				_structureDirty = true;
				return false;
			}
			if (hasGroupClip && canFlattenGroupClip)
			{
				properties.PhysicalClip = D2D1::RectF(
					groupClipRootDip.left * dpiScale,
					(groupClipRootDip.top
						+ static_cast<float>(titleBarOffsetDip)) * dpiScale,
					groupClipRootDip.right * dpiScale,
					(groupClipRootDip.bottom
						+ static_cast<float>(titleBarOffsetDip)) * dpiScale);
				properties.HasClip = true;
			}
			else if (hasGroupClip)
			{
				properties.TransformedClipChain.reserve(groupClipChain.size());
				for (const auto& clip : groupClipChain)
				{
					auto physicalTransform =
						cui::dcomp_detail::DipTransformToPhysicalPixels(
							clip.LocalToRoot, dpiScale,
							static_cast<float>(titleBarOffsetDip) * dpiScale);
					properties.TransformedClipChain.push_back({
						D2D1::RectF(
							clip.LocalRect.left * dpiScale,
							clip.LocalRect.top * dpiScale,
							clip.LocalRect.right * dpiScale,
							clip.LocalRect.bottom * dpiScale),
						clip.RadiusX * dpiScale,
						clip.RadiusY * dpiScale,
						physicalTransform });
				}
			}
		}
	}
	catch (...)
	{
		for (auto& item : _segments) item.Context = nullptr;
		return false;
	}
	if (!host.StageSceneLayerGroups(_groupPropertiesScratch))
	{
		for (auto& item : _segments) item.Context = nullptr;
		return false;
	}
	preparation.GroupStageMicroseconds += groupClock.ElapsedMicroseconds();
	if (!_hasPreparedRevision || _preparedRevision != _revision)
	{
		// Segment ownership can change while its count stays constant. No pixels
		// from the previous topology may leak into the new retained snapshot.
		host.InvalidateFrameHistory();
		_preparedRevision = _revision;
		_hasPreparedRevision = true;
	}
	_preparedStatistics = preparation;
	return true;
}

bool PresentationScene::RenderComposition(
	PresentationRenderHost& host,
	PresentationRenderHost::FrameTransaction& transaction,
	const RECT& contentDirty,
	int titleBarOffset,
	float logicalWidth,
	float logicalHeight)
{
	(void)logicalWidth;
	(void)logicalHeight;
	if (!host.UsesComposition()
		|| !host.IsTransactionActive(transaction)
		|| contentDirty.right <= contentDirty.left
		|| contentDirty.bottom <= contentDirty.top) return false;
	BeginFrameStatistics();
	_frameStatistics.Preparation = _preparedStatistics;
	_frameStatistics.Transaction = transaction.Sequence;
	_frameStatistics.ResourceGeneration = transaction.ResourceGeneration;
	ApplyPendingGeometryInvalidations();
	const bool hasGeometryRasterGroup = std::any_of(
		_segments.begin(), _segments.end(), [](const Segment& segment)
		{
			return segment.GeometryRasterMembers.size() > 1u;
		});
	try
	{
		_segmentSubmissionScratch.assign(_segments.size(), uint8_t{ 0 });
		if (_preparedNodeScratch.size() != _nodes.size())
			_preparedNodeScratch.assign(_nodes.size(), uint8_t{ 0 });
		if (hasGeometryRasterGroup)
		{
			_geometryRasterDamageScratch.assign(_segments.size(), RECT{});
			_geometryRasterPartialScratch.assign(
				_segments.size(), uint8_t{ 0 });
			_geometryRasterBoundsScratch.assign(
				_nodes.size(), D2D1_RECT_F{});
			_geometryRasterBoundsValidScratch.assign(
				_nodes.size(), uint8_t{ 0 });
		}
		else
		{
			_geometryRasterDamageScratch.clear();
			_geometryRasterPartialScratch.clear();
			_geometryRasterBoundsScratch.clear();
			_geometryRasterBoundsValidScratch.clear();
		}
	}
	catch (...)
	{
		return false;
	}

	struct RenderingScope
	{
		bool& Value;
		explicit RenderingScope(bool& value) : Value(value) { Value = true; }
		~RenderingScope() { Value = false; }
	} rendering(_rendering);

	RECT clientDirty = contentDirty;
	clientDirty.top += titleBarOffset;
	clientDirty.bottom += titleBarOffset;
	size_t activeSegment = NoSegment;
	D2DGraphics* segmentContext = nullptr;
	PresentationRenderHost::SurfaceFrame segmentFrame;
	size_t activeAncestorGeometryClipCount = 0;
	struct ActiveOpacityGroup
	{
		size_t End = 0;
		D2DGraphics* Context = nullptr;
	};
	std::vector<ActiveOpacityGroup> activeOpacityGroups;
	try { activeOpacityGroups.reserve(_nodes.size()); }
	catch (...) { return false; }
	bool frameHealthy = true;
	// 0 = not inspected, 1 = raster submission required, 2 = the retained
	// command lists are complete and this frame can publish visual properties
	// without opening the segment's swap chain, 3 = preparation proved that the
	// unmaterialized logical segment is wholly outside the viewport.
	auto& segmentSubmission = _segmentSubmissionScratch;
	auto& preparedNodes = _preparedNodeScratch;
	// A shared arbitrary-Geometry surface can use a partial Composition Surface
	// update only when every node already has a current-generation command list
	// and the change is confined to content/geometry bounds. The damage is the
	// union of old and new root-space bounds for every dirty node. Rendering later
	// replays all current nodes intersecting this rect, so clearing moved pixels
	// also restores any lower z-order content they previously covered.
	auto addGeometryDamage = [](RECT& accumulated, bool& hasDamage, RECT value)
	{
		if (value.right <= value.left || value.bottom <= value.top) return;
		if (!hasDamage)
		{
			accumulated = value;
			hasDamage = true;
			return;
		}
		accumulated.left = (std::min)(accumulated.left, value.left);
		accumulated.top = (std::min)(accumulated.top, value.top);
		accumulated.right = (std::max)(accumulated.right, value.right);
		accumulated.bottom = (std::max)(accumulated.bottom, value.bottom);
	};
	for (size_t segmentIndex = 0; hasGeometryRasterGroup
		&& segmentIndex < _segments.size(); ++segmentIndex)
	{
		auto& segment = _segments[segmentIndex];
		if (segment.GeometryRasterMembers.size() < 2u
			|| !segment.Context
			|| segment.Context->RequiresFullPresentFrame()) continue;
		bool valid = true;
		bool dirty = false;
		bool hasDamage = false;
		RECT damage{};
		const auto origin = segment.SurfaceOriginDip;
		auto surfaceRect = [&](D2D1_RECT_F bounds)
		{
			bounds.left -= origin.x;
			bounds.right -= origin.x;
			bounds.top += static_cast<float>(titleBarOffset) - origin.y;
			bounds.bottom += static_cast<float>(titleBarOffset) - origin.y;
			return ToRect(bounds, 2);
		};
		const size_t end = (std::min)(segment.NodeEnd, _nodes.size());
		for (size_t nodeIndex = (std::min)(segment.NodeStart, end);
			valid && nodeIndex < end; ++nodeIndex)
		{
			auto& node = _nodes[nodeIndex];
			auto* control = node.Element.Get();
			if (!preparedNodes[nodeIndex] || !control || !control->IsVisible
				|| !RefreshNodeState(node) || !node.HasPresented
				|| !node.HasGeometry || !node.DrawingCommands
				|| node.CommandGeneration != transaction.ResourceGeneration
				|| node.CompositionDirty)
			{
				valid = false;
				break;
			}
			const auto current = control->GetRenderedAbsoluteRectDip();
			if (!FiniteRect(current))
			{
				valid = false;
				break;
			}
			_geometryRasterBoundsScratch[nodeIndex] = current;
			_geometryRasterBoundsValidScratch[nodeIndex] = uint8_t{ 1 };
			if (!node.GeometryDirty && !SameRect(current, node.RenderedBounds))
			{
				valid = false;
				break;
			}
			if (!node.ContentDirty && !node.GeometryDirty) continue;
			dirty = true;
			addGeometryDamage(
				damage, hasDamage, surfaceRect(node.RenderedBounds));
			addGeometryDamage(damage, hasDamage, surfaceRect(current));
		}
		if (!valid || !dirty || !hasDamage) continue;
		RECT clipped{};
		if (!::IntersectRect(
			&clipped, &damage, &segment.LogicalSurfaceClient)) continue;
		const bool full = clipped.left <= segment.LogicalSurfaceClient.left
			&& clipped.top <= segment.LogicalSurfaceClient.top
			&& clipped.right >= segment.LogicalSurfaceClient.right
			&& clipped.bottom >= segment.LogicalSurfaceClient.bottom;
		if (full) continue;
		_geometryRasterDamageScratch[segmentIndex] = clipped;
		_geometryRasterPartialScratch[segmentIndex] = uint8_t{ 1 };
	}
	auto popOpacityGroups = [&](size_t nodeIndex, bool all)
	{
		while (!activeOpacityGroups.empty()
			&& (all || nodeIndex >= activeOpacityGroups.back().End))
		{
			if (activeOpacityGroups.back().Context)
				activeOpacityGroups.back().Context->PopOpacity();
			activeOpacityGroups.pop_back();
		}
	};

	auto endSegment = [&]
	{
		if (!segmentContext) return;
		// Opacity is the innermost retained group. Resolve it before ancestor
		// geometry masks and before ending the D2D surface draw.
		popOpacityGroups(_nodes.size(), true);
		while (activeAncestorGeometryClipCount > 0u)
		{
			segmentContext->PopGeometryClip();
			--activeAncestorGeometryClipCount;
		}
		const SceneWorkClock closeClock;
		const bool closed = host.CloseSurface(transaction, segmentFrame);
		_frameStatistics.SceneSurfaceCloseMicroseconds +=
			closeClock.ElapsedMicroseconds();
		_frameStatistics.SceneSurfaceEndDrawMicroseconds +=
			segmentFrame.EndDrawMicroseconds;
		_frameStatistics.SceneSurfacePresentMicroseconds +=
			segmentFrame.PresentMicroseconds;
		_frameStatistics.SceneSurfaceSubmitMicroseconds +=
			segmentFrame.SurfaceSubmitMicroseconds;
		if (!closed)
			frameHealthy = false;
		segmentContext = nullptr;
		activeSegment = NoSegment;
	};
	auto beginSegment = [&](size_t index)
	{
		if (!frameHealthy) return false;
		if (index == activeSegment) return true;
		endSegment();
		if (!frameHealthy || index >= _segments.size()
			|| !_segments[index].Context) return false;
		activeSegment = index;
		segmentContext = _segments[index].Context;
		const bool geometryPartial = index < _geometryRasterPartialScratch.size()
			&& _geometryRasterPartialScratch[index] != 0u;
		const RECT surfaceDirty = geometryPartial
			? _geometryRasterDamageScratch[index]
			: (_segments[index].IsolationRoot
				? _segments[index].LogicalSurfaceClient : clientDirty);
		const RECT surfaceClient = _segments[index].IsolationRoot
			? _segments[index].LogicalSurfaceClient
			: transaction.LogicalClient;
		const SceneWorkClock openClock;
		const bool opened = host.OpenSceneSurface(
			transaction, segmentContext, surfaceDirty,
			surfaceClient, segmentFrame);
		_frameStatistics.SceneSurfaceOpenMicroseconds +=
			openClock.ElapsedMicroseconds();
		if (!opened)
		{
			segmentContext = nullptr;
			activeSegment = NoSegment;
			frameHealthy = false;
			return false;
		}
		if (_segments[index].IsolationRoot)
		{
			const auto origin = _segments[index].SurfaceOriginDip;
			segmentContext->SetTransform(
				D2D1::Matrix3x2F::Translation(-origin.x, -origin.y));
			if (_segments[index].RasterizesAncestorGeometryClip)
			{
				for (const auto& clip :
					_segments[index].AncestorGeometryClips)
				{
					if (!clip.NativeGeometry
						|| !segmentContext->PushGeometryClip(
							clip.NativeGeometry.Get()))
					{
						frameHealthy = false;
						return false;
					}
					++_frameStatistics.AncestorGeometryMaskLayerPushCount;
					++activeAncestorGeometryClipCount;
				}
			}
		}
		if (_segments[index].GeometryRasterMembers.size() > 1u)
		{
			if (geometryPartial)
			{
				++_frameStatistics.GeometryRasterPartialUpdateCount;
				const uint64_t width = static_cast<uint64_t>(
					(std::max)(LONG{ 0 },
						surfaceDirty.right - surfaceDirty.left));
				const uint64_t height = static_cast<uint64_t>(
					(std::max)(LONG{ 0 },
						surfaceDirty.bottom - surfaceDirty.top));
				const uint64_t area = width != 0u
					&& height > UINT64_MAX / width
					? UINT64_MAX : width * height;
				_frameStatistics.GeometryRasterPartialDamageArea =
					area > UINT64_MAX
						- _frameStatistics.GeometryRasterPartialDamageArea
					? UINT64_MAX
					: _frameStatistics.GeometryRasterPartialDamageArea + area;
			}
			else ++_frameStatistics.GeometryRasterFullReplayCount;
		}
		++_frameStatistics.SceneSurfacesOpened;
		return true;
	};

	for (size_t nodeIndex = 0; nodeIndex < _nodes.size(); ++nodeIndex)
	{
		popOpacityGroups(nodeIndex, false);
		auto& node = _nodes[nodeIndex];
		if (!frameHealthy || transaction.Failed) break;
		if (node.Overlay && !node.OverlaySegmented) continue;
		if (node.SegmentIndex < _segments.size())
		{
			const auto& preparedSegment = _segments[node.SegmentIndex];
			if (preparedSegment.IsolationRoot && !preparedSegment.Context)
			{
				// PrepareComposition has already published layout/state and proved
				// that this logical segment needs no physical layer. Preserve the
				// unpresented dirty node for exact first recording on viewport entry;
				// repeating geometry/clip culling here only scales CPU with offscreen
				// logical slots and cannot change this frame's submission decision.
				if (segmentSubmission[node.SegmentIndex] == 0u)
				{
					endSegment();
					if (!frameHealthy) break;
					segmentSubmission[node.SegmentIndex] = uint8_t{ 3 };
				}
				if (node.ContentDirty) ++_frameStatistics.ContentDirtyNodes;
				if (node.GeometryDirty) ++_frameStatistics.GeometryDirtyNodes;
				if (node.CompositionDirty)
					++_frameStatistics.CompositionDirtyNodes;
				++_frameStatistics.CulledNodes;
				++_frameStatistics.PreSurfaceCulledNodes;
				continue;
			}
		}
		if (node.SegmentIndex < _segments.size()
			&& _segments[node.SegmentIndex].IsolationRoot
			&& segmentSubmission[node.SegmentIndex] == 0)
		{
			// Preserve segment order: finish the preceding surface, then let every
			// isolated node publish deferred presentation state before deciding
			// whether the segment can reuse its already-presented pixels wholesale.
			endSegment();
			if (!frameHealthy) break;
			const auto& candidateSegment = _segments[node.SegmentIndex];
			const size_t end = (std::min)(
				candidateSegment.NodeEnd, _nodes.size());
			bool compositionOnly = end > candidateSegment.NodeStart;
			for (size_t candidateIndex = (std::min)(
				candidateSegment.NodeStart, end);
				candidateIndex < end; ++candidateIndex)
			{
				auto& candidate = _nodes[candidateIndex];
				Control* prepared = candidate.Element.Get();
				const bool reusedPreparation =
					preparedNodes[candidateIndex] != 0;
				if (candidate.NativeComposition
					|| (candidate.Overlay && !candidate.OverlaySegmented)
					|| (reusedPreparation
						? (!prepared || !RefreshNodeState(candidate))
						: !PrepareNodeForRendering(candidate, prepared)))
				{
					compositionOnly = false;
					continue;
				}
				preparedNodes[candidateIndex] = uint8_t{ 1 };
			}
			// PreparePresentation may have queued a range invalidation. Classify
			// only after those ranges are folded into the retained snapshot.
			ApplyPendingGeometryInvalidations();
			for (size_t candidateIndex = (std::min)(
				candidateSegment.NodeStart, end);
				candidateIndex < end; ++candidateIndex)
			{
				auto& candidate = _nodes[candidateIndex];
				auto* prepared = candidate.Element.Get();
				const bool opacityOnlyCompositionDirty =
					candidate.CompositionDirty && prepared
					&& (std::any_of(
						candidateSegment.OpacityIsolationRoots.begin(),
						candidateSegment.OpacityIsolationRoots.end(),
						[prepared](const ControlWeakReference& root)
						{ return root.Get() == prepared; })
						|| std::any_of(
							candidateSegment.OpacityGroupRoots.begin(),
							candidateSegment.OpacityGroupRoots.end(),
							[prepared](const ControlWeakReference& root)
							{ return root.Get() == prepared; }));
				if (!preparedNodes[candidateIndex] || !prepared
					|| !prepared->IsVisible || !RefreshNodeState(candidate)
					|| !candidate.HasPresented || !candidate.DrawingCommands
					|| candidate.CommandGeneration
						!= transaction.ResourceGeneration
					|| candidate.ContentDirty
					|| (candidate.CompositionDirty
						&& !opacityOnlyCompositionDirty)
					|| (candidateSegment.RasterizesAncestorGeometryClip
						&& candidate.GeometryDirty)
					|| (candidate.GeometryDirty
						&& !candidate.TransformOnlyGeometryDirty))
					compositionOnly = false;
			}
			segmentSubmission[node.SegmentIndex] =
				compositionOnly ? uint8_t{ 2 } : uint8_t{ 1 };
			if (compositionOnly)
				++_frameStatistics.CompositionOnlySegments;
		}
		Control* control = nullptr;
		if (preparedNodes[nodeIndex])
		{
			control = node.Element.Get();
			if (!control || !RefreshNodeState(node)) continue;
		}
		else if (!PrepareNodeForRendering(node, control)) continue;
		if (node.NativeComposition)
		{
			endSegment();
			if (!frameHealthy) break;
			const bool contentDirtyNode = node.ContentDirty;
			const bool geometryDirtyNode = node.GeometryDirty;
			const bool compositionDirtyNode = node.CompositionDirty;
			if (contentDirtyNode) ++_frameStatistics.ContentDirtyNodes;
			if (geometryDirtyNode) ++_frameStatistics.GeometryDirtyNodes;
			if (compositionDirtyNode)
				++_frameStatistics.CompositionDirtyNodes;
			if (!RefreshNodeGeometry(node)) continue;
			control = node.Element.Get();
			if (!control) continue;
			if (!control->IsVisible)
			{
				++_frameStatistics.CulledNodes;
				continue;
			}
			if (!node.HasPresented || contentDirtyNode
				|| geometryDirtyNode || compositionDirtyNode)
			{
				const auto submitted = control->GetPresentationRevisions();
				control->SetPresentationOrderOverride(node.Order);
				control->OnRender();
				control->ClearPresentationOrderOverride();
				++_frameStatistics.NativeCommitNodes;
				(void)CompleteNode(node, submitted);
			}
			continue;
		}

		const bool compositionOnly = node.SegmentIndex < _segments.size()
			&& segmentSubmission[node.SegmentIndex] == 2;
		if (compositionOnly)
		{
			const bool contentDirtyNode = node.ContentDirty;
			const bool geometryDirtyNode = node.GeometryDirty;
			const bool compositionDirtyNode = node.CompositionDirty;
			if (contentDirtyNode) ++_frameStatistics.ContentDirtyNodes;
			if (geometryDirtyNode) ++_frameStatistics.GeometryDirtyNodes;
			if (compositionDirtyNode)
				++_frameStatistics.CompositionDirtyNodes;
			if (!RefreshNodeGeometry(node)) continue;
			control = node.Element.Get();
			if (!control) continue;
			if (geometryDirtyNode && node.CompositionIsolated)
				++_frameStatistics.CompositionTransformOnlyNodes;
			++_frameStatistics.CommandCacheHitNodes;
			const auto submitted = control->GetPresentationRevisions();
			(void)CompleteNode(node, submitted);
			continue;
		}

		const bool contentDirtyNode = node.ContentDirty;
		const bool geometryDirtyNode = node.GeometryDirty;
		const bool transformOnlyGeometry =
			node.TransformOnlyGeometryDirty && node.CompositionIsolated;
		const bool compositionDirtyNode = node.CompositionDirty;
		if (contentDirtyNode) ++_frameStatistics.ContentDirtyNodes;
		if (geometryDirtyNode) ++_frameStatistics.GeometryDirtyNodes;
		if (compositionDirtyNode)
			++_frameStatistics.CompositionDirtyNodes;
		const bool geometryPartial = node.SegmentIndex < _segments.size()
			&& node.SegmentIndex < _geometryRasterPartialScratch.size()
			&& _geometryRasterPartialScratch[node.SegmentIndex] != 0u;
		if (geometryPartial && !contentDirtyNode
			&& !geometryDirtyNode && !compositionDirtyNode)
		{
			if (!_geometryRasterBoundsValidScratch[nodeIndex])
			{
				frameHealthy = false;
				break;
			}
			const auto& segment = _segments[node.SegmentIndex];
			auto bounds = _geometryRasterBoundsScratch[nodeIndex];
			bounds.left -= segment.SurfaceOriginDip.x;
			bounds.right -= segment.SurfaceOriginDip.x;
			bounds.top += static_cast<float>(titleBarOffset)
				- segment.SurfaceOriginDip.y;
			bounds.bottom += static_cast<float>(titleBarOffset)
				- segment.SurfaceOriginDip.y;
			const RECT nodeDamageBounds = ToRect(bounds, 2);
			if (!RectIntersects(
				nodeDamageBounds,
				_geometryRasterDamageScratch[node.SegmentIndex]))
			{
				++_frameStatistics.GeometryRasterPartialSkippedNodes;
				continue;
			}
		}
		if (!RefreshNodeGeometry(node)) continue;
		control = node.Element.Get();
		if (!control) continue;
		if (!control->IsVisible)
		{
			++_frameStatistics.CulledNodes;
			if (activeSegment != node.SegmentIndex)
				++_frameStatistics.PreSurfaceCulledNodes;
			continue;
		}
		const auto* activePropertiesSegment =
			node.SegmentIndex < _segments.size()
			? &_segments[node.SegmentIndex] : nullptr;
		const bool stagesOpacity = activePropertiesSegment
			&& (std::any_of(
				activePropertiesSegment->OpacityIsolationRoots.begin(),
				activePropertiesSegment->OpacityIsolationRoots.end(),
				[control](const ControlWeakReference& root)
				{ return root.Get() == control; })
				|| std::any_of(
					activePropertiesSegment->OpacityGroupRoots.begin(),
					activePropertiesSegment->OpacityGroupRoots.end(),
					[control](const ControlWeakReference& root)
					{ return root.Get() == control; }));
		const double opacity = control->GetOpacity();
		if (!std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0)
		{
			frameHealthy = false;
			break;
		}
		const bool startsRasterOpacityGroup =
			!stagesOpacity && opacity != 1.0;
		RECT controlRect = ToRect(node.RenderedBounds, 2);
		controlRect.top += titleBarOffset;
		controlRect.bottom += titleBarOffset;
		RECT clientClip{};
		const bool fullIsolationReplay = activePropertiesSegment
			&& activePropertiesSegment->IsolationRoot && !geometryPartial;
		RECT replayDamage = contentDirty;
		if (fullIsolationReplay)
		{
			// An ordinary isolated segment currently opens and clears its complete
			// backing surface.  The HWND dirty rectangle may cover only a hovered
			// MenuItem/ComboBoxItem; using that narrow rectangle to cull siblings
			// would then erase their retained text from the freshly cleared surface.
			// Replay every visible command list that can contribute to the full
			// surface.  Shared-Geometry segments keep their exact partial path.
			replayDamage = transaction.LogicalClient;
			replayDamage.top -= titleBarOffset;
			replayDamage.bottom -= titleBarOffset;
		}
		const bool culled =
			!GetClientClip(control, replayDamage, titleBarOffset, clientClip)
			|| !RectIntersects(clientClip, controlRect);
		// An isolated surface with no retained pixels must not be opened and
		// presented merely to discover that its node is outside the current
		// client clip. Preserve the old ordering for opacity-group roots: even
		// when the root has no own pixels, its visible descendants still need the
		// group pushed before their first draw.
		if (culled && !startsRasterOpacityGroup)
		{
			++_frameStatistics.CulledNodes;
			if (activeSegment != node.SegmentIndex)
				++_frameStatistics.PreSurfaceCulledNodes;
			continue;
		}
		if (!beginSegment(node.SegmentIndex)) break;
		if (startsRasterOpacityGroup)
		{
			const size_t opacityEnd = (std::min)(
				node.SubtreeEnd, _nodes.size());
			if (!activePropertiesSegment
				|| opacityEnd > activePropertiesSegment->NodeEnd
				|| !segmentContext->PushOpacity(static_cast<float>(opacity)))
			{
				frameHealthy = false;
				break;
			}
			activeOpacityGroups.push_back({ opacityEnd, segmentContext });
			++_frameStatistics.OpacityLayerPushCount;
		}
		if (culled)
		{
			++_frameStatistics.CulledNodes;
			continue;
		}

		if (transformOnlyGeometry)
			++_frameStatistics.CompositionTransformOnlyNodes;
		const auto submitted = control->GetPresentationRevisions();
		const bool needsRecording = !node.DrawingCommands
			|| node.CommandGeneration != transaction.ResourceGeneration
			|| contentDirtyNode
			|| (geometryDirtyNode && !transformOnlyGeometry);
		if (needsRecording)
		{
			Microsoft::WRL::ComPtr<ID2D1CommandList> commands;
			const ControlWeakReference renderTarget(control);
			const Segment* recordingSegment = node.SegmentIndex < _segments.size()
				? &_segments[node.SegmentIndex] : nullptr;
			const std::vector<ControlWeakReference>* recordingIsolationRoots =
				recordingSegment ? &recordingSegment->IsolationRoots : nullptr;
			if (recordingSegment
				&& node.GeometryRasterMemberIndex
					< recordingSegment->GeometryRasterMembers.size())
				recordingIsolationRoots = &recordingSegment->
					GeometryRasterMembers[node.GeometryRasterMemberIndex].
					IsolationRoots;
			const SceneWorkClock recordClock;
			const bool recorded = host.RecordDrawingCommands(
				transaction, segmentContext,
				[renderTarget, recordingIsolationRoots]
				{
					auto* liveTarget = renderTarget.Get();
					if (!liveTarget) return;
					const auto* isolationRoots = recordingIsolationRoots;
					if (isolationRoots && isolationRoots->size() == 1u)
					{
						auto* liveRoot = isolationRoots->front().Get();
						if (!liveRoot) return;
						cui::framework::PresentationAccess::
							RenderTransformSuppressionScope suppress(
								std::span<Control* const>{ &liveRoot, 1u });
						liveTarget->OnRender();
						return;
					}
					if (isolationRoots && !isolationRoots->empty())
					{
						std::vector<Control*> liveRoots;
						liveRoots.reserve(isolationRoots->size());
						for (const auto& weakRoot : *isolationRoots)
						{
							auto* root = weakRoot.Get();
							if (!root) return;
							liveRoots.push_back(root);
						}
						cui::framework::PresentationAccess::
							RenderTransformSuppressionScope suppress(
								std::span<Control* const>{
									liveRoots.data(), liveRoots.size() });
						liveTarget->OnRender();
					}
					else liveTarget->OnRender();
				}, commands.ReleaseAndGetAddressOf());
			_frameStatistics.SceneCommandRecordMicroseconds +=
				recordClock.ElapsedMicroseconds();
			if (!recorded)
			{
				frameHealthy = false;
				break;
			}
			node.DrawingCommands = std::move(commands);
			node.CommandGeneration = transaction.ResourceGeneration;
			++_frameStatistics.CommandRecordedNodes;
		}
		else
		{
			++_frameStatistics.CommandCacheHitNodes;
		}

		RECT replayClip = clientClip;
		if (node.SegmentIndex < _segments.size()
			&& _segments[node.SegmentIndex].IsolationRoot)
		{
			const auto& segment = _segments[node.SegmentIndex];
			// External ancestor clips are owned by the DComp parent chain. Baking
			// their root-space AABB into the isolated swap chain would move that
			// clip with later composition-only target transforms. Restrict replay
			// only to the retained surface's base root-space extent; clips inside
			// the isolated subtree remain part of each recorded command list.
			const auto origin = segment.SurfaceOriginDip;
			replayClip = ToRect(D2D1::RectF(
				origin.x,
				origin.y,
				origin.x + static_cast<float>(
					segment.LogicalSurfaceClient.right),
				origin.y + static_cast<float>(
					segment.LogicalSurfaceClient.bottom)));
		}
		segmentContext->PushDrawRect(
			static_cast<float>(replayClip.left),
			static_cast<float>(replayClip.top),
			static_cast<float>(replayClip.right - replayClip.left),
			static_cast<float>(replayClip.bottom - replayClip.top));
		const Segment* replaySegment = node.SegmentIndex < _segments.size()
			? &_segments[node.SegmentIndex] : nullptr;
		if (replaySegment && replaySegment->RasterizesAncestorGeometryClip)
		{
			const auto origin = replaySegment->SurfaceOriginDip;
			auto logical = replaySegment->LogicalTransform;
			if (node.GeometryRasterMemberIndex
				< replaySegment->GeometryRasterMembers.size())
				logical = replaySegment->GeometryRasterMembers[
					node.GeometryRasterMemberIndex].LogicalTransform;
			segmentContext->SetTransform(
				AsMatrix(logical)
					* D2D1::Matrix3x2F::Translation(-origin.x, -origin.y));
		}
		const SceneWorkClock replayClock;
		const bool replayed = host.ReplayDrawingCommands(
			transaction, segmentContext, node.DrawingCommands.Get());
		_frameStatistics.SceneCommandReplayMicroseconds +=
			replayClock.ElapsedMicroseconds();
		if (replaySegment && replaySegment->RasterizesAncestorGeometryClip)
		{
			const auto origin = replaySegment->SurfaceOriginDip;
			segmentContext->SetTransform(
				D2D1::Matrix3x2F::Translation(-origin.x, -origin.y));
		}
		segmentContext->PopDrawRect();
		if (!replayed)
		{
			frameHealthy = false;
			break;
		}
		++_frameStatistics.CommandReplayedNodes;
		if (geometryPartial)
			++_frameStatistics.GeometryRasterPartialReplayNodes;
		if (!contentDirtyNode && !geometryDirtyNode
			&& !compositionDirtyNode)
			++_frameStatistics.DamageReplayNodes;
		(void)CompleteNode(node, submitted);
	}
	endSegment();
	return frameHealthy && !transaction.Failed;
}

bool PresentationScene::RenderRaster(const RECT& contentDirty)
{
	if (contentDirty.right <= contentDirty.left
		|| contentDirty.bottom <= contentDirty.top) return false;
	BeginFrameStatistics();
	_frameStatistics.ResourceGeneration = _resourceGeneration;
	ApplyPendingGeometryInvalidations();
	struct RenderingScope
	{
		bool& Value;
		explicit RenderingScope(bool& value) : Value(value) { Value = true; }
		~RenderingScope() { Value = false; }
	} rendering(_rendering);

	// Raster fallback and composition now share the same retained traversal:
	// every scene node draws only itself.  Parent controls never own recursive
	// painting, so switching render hosts cannot change visual semantics.
	struct ActiveOpacityGroup
	{
		size_t End = 0;
		D2DGraphics* Context = nullptr;
	};
	std::vector<ActiveOpacityGroup> opacityGroups;
	try { opacityGroups.reserve(_nodes.size()); }
	catch (...) { return false; }
	bool frameHealthy = true;
	auto popOpacityGroups = [&](size_t nodeIndex)
		{
			while (!opacityGroups.empty()
				&& nodeIndex >= opacityGroups.back().End)
			{
				if (opacityGroups.back().Context)
					opacityGroups.back().Context->PopOpacity();
				opacityGroups.pop_back();
			}
		};
	for (size_t nodeIndex = 0; nodeIndex < _nodes.size(); ++nodeIndex)
	{
		popOpacityGroups(nodeIndex);
		if (!frameHealthy) break;
		auto& node = _nodes[nodeIndex];
		if (node.Overlay) continue;
		Control* control = nullptr;
		if (!PrepareNodeForRendering(node, control)
			|| !control->IsVisible) continue;
		auto* drawingContext = control->GetDrawingContext();
		if (!drawingContext) continue;
		const double opacity = control->GetOpacity();
		if (!std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0)
		{
			frameHealthy = false;
			break;
		}
		if (opacity != 1.0)
		{
			if (!drawingContext->PushOpacity(static_cast<float>(opacity)))
			{
				frameHealthy = false;
				break;
			}
			opacityGroups.push_back({
				(std::min)(node.SubtreeEnd, _nodes.size()), drawingContext });
			++_frameStatistics.OpacityLayerPushCount;
		}
		const bool geometryDirtyNode = node.GeometryDirty;
		if (node.ContentDirty) ++_frameStatistics.ContentDirtyNodes;
		if (geometryDirtyNode) ++_frameStatistics.GeometryDirtyNodes;
		if (node.CompositionDirty)
			++_frameStatistics.CompositionDirtyNodes;
		if (!node.ContentDirty && !geometryDirtyNode
			&& !node.CompositionDirty)
			++_frameStatistics.DamageReplayNodes;
		if (!RefreshNodeGeometry(node)) continue;
		control = node.Element.Get();
		if (!control) continue;
		const RECT controlRect = ToRect(node.RenderedBounds, 2);
		RECT clientClip{};
		if (!GetClientClip(control, contentDirty, 0, clientClip)
			|| !RectIntersects(clientClip, controlRect))
		{
			++_frameStatistics.CulledNodes;
			continue;
		}
		const auto submitted = control->GetPresentationRevisions();
		drawingContext->PushDrawRect(
			static_cast<float>(clientClip.left),
			static_cast<float>(clientClip.top),
			static_cast<float>(clientClip.right - clientClip.left),
			static_cast<float>(clientClip.bottom - clientClip.top));
		control->OnRender();
		drawingContext->PopDrawRect();
		++_frameStatistics.ImmediateDrawNodes;
		(void)CompleteNode(node, submitted);
	}
	popOpacityGroups(_nodes.size());
	return frameHealthy;
}

bool PresentationScene::RequiresOverlayFrame()
{
	ApplyPendingGeometryInvalidations();
	if (_overlaySurfaceDirty) return true;
	for (auto& node : _nodes)
	{
		if (!node.Overlay || node.OverlaySegmented
			|| !RefreshNodeState(node)) continue;
		if (!node.HasPresented || node.ContentDirty || node.GeometryDirty
			|| node.CompositionDirty) return true;
	}
	return false;
}

bool PresentationScene::RenderOverlay(
	const RECT& contentDirty,
	bool includeCompositionSegments)
{
	if (contentDirty.right <= contentDirty.left
		|| contentDirty.bottom <= contentDirty.top) return false;
	struct RenderingScope
	{
		bool& Value;
		explicit RenderingScope(bool& value) : Value(value) { Value = true; }
		~RenderingScope() { Value = false; }
	} rendering(_rendering);

	struct ActiveOpacityGroup
	{
		size_t End = 0;
		D2DGraphics* Context = nullptr;
	};
	std::vector<ActiveOpacityGroup> opacityGroups;
	try { opacityGroups.reserve(_nodes.size()); }
	catch (...) { return false; }
	auto popOpacityGroups = [&](size_t nodeIndex)
	{
		while (!opacityGroups.empty()
			&& nodeIndex >= opacityGroups.back().End)
		{
			if (opacityGroups.back().Context)
				opacityGroups.back().Context->PopOpacity();
			opacityGroups.pop_back();
		}
	};
	bool frameHealthy = true;
	for (size_t nodeIndex = 0; nodeIndex < _nodes.size(); ++nodeIndex)
	{
		popOpacityGroups(nodeIndex);
		auto& node = _nodes[nodeIndex];
		if (!node.Overlay
			|| (node.OverlaySegmented && !includeCompositionSegments)) continue;
		Control* control = nullptr;
		if (!PrepareNodeForRendering(node, control)) continue;
		const bool contentDirtyNode = node.ContentDirty;
		const bool geometryDirtyNode = node.GeometryDirty;
		const bool compositionDirtyNode = node.CompositionDirty;
		if (contentDirtyNode) ++_frameStatistics.ContentDirtyNodes;
		if (geometryDirtyNode) ++_frameStatistics.GeometryDirtyNodes;
		if (compositionDirtyNode)
			++_frameStatistics.CompositionDirtyNodes;
		const auto submitted = control->GetPresentationRevisions();
		if (!control->IsVisible)
		{
			(void)CompleteNode(node, submitted);
			continue;
		}
		if (!RefreshNodeGeometry(node)) continue;
		control = node.Element.Get();
		if (!control) continue;
		D2DGraphics* drawingContext = nullptr;
		if (node.NativeComposition)
		{
			// Segmented native content is handled by its composition lease and was
			// skipped above. The legacy raster fallback can preserve an ungrouped
			// native visual, but cannot apply raster subtree opacity to that visual.
			if (!opacityGroups.empty() || control->GetOpacity() != 1.0)
			{
				frameHealthy = false;
				break;
			}
		}
		else
		{
			drawingContext = control->GetDrawingContext();
			if (!drawingContext) continue;
			const double opacity = control->GetOpacity();
			if (!std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0)
			{
				frameHealthy = false;
				break;
			}
			if (opacity != 1.0)
			{
				if (!drawingContext->PushOpacity(static_cast<float>(opacity)))
				{
					frameHealthy = false;
					break;
				}
				opacityGroups.push_back({
					(std::min)(node.SubtreeEnd, _nodes.size()),
					drawingContext });
				++_frameStatistics.OpacityLayerPushCount;
			}
		}
		const RECT controlRect = ToRect(node.RenderedBounds, 2);
		RECT clientClip{};
		if (!GetClientClip(control, contentDirty, 0, clientClip)
			|| !RectIntersects(clientClip, controlRect))
		{
			++_frameStatistics.CulledNodes;
			continue;
		}

		if (node.NativeComposition)
		{
			if (!node.HasPresented || contentDirtyNode
				|| geometryDirtyNode || compositionDirtyNode)
			{
				control->SetPresentationOrderOverride(node.Order);
				control->OnRender();
				control->ClearPresentationOrderOverride();
				++_frameStatistics.NativeCommitNodes;
				(void)CompleteNode(node, submitted);
			}
			continue;
		}

		drawingContext->PushDrawRect(
			static_cast<float>(clientClip.left),
			static_cast<float>(clientClip.top),
			static_cast<float>(clientClip.right - clientClip.left),
			static_cast<float>(clientClip.bottom - clientClip.top));
		control->OnRender();
		drawingContext->PopDrawRect();
		++_frameStatistics.ImmediateDrawNodes;
		(void)CompleteNode(node, submitted);
	}
	popOpacityGroups(_nodes.size());
	if (frameHealthy) _overlaySurfaceDirty = false;
	return frameHealthy;
}

bool PresentationScene::TryGetNodeSnapshot(
	const Control* control,
	PresentationNodeSnapshot& out) const noexcept
{
	out = {};
	if (!control) return false;
	const auto found = _nodeIndex.find(const_cast<Control*>(control));
	if (found == _nodeIndex.end() || found->second >= _nodes.size())
		return false;
	const auto& node = _nodes[found->second];
	out.ContentDirty = node.ContentDirty;
	out.GeometryDirty = node.GeometryDirty;
	out.CompositionDirty = node.CompositionDirty;
	out.HasGeometry = node.HasGeometry;
	out.HasPresented = node.HasPresented;
	out.HasDrawingCommands = node.DrawingCommands != nullptr;
	out.NativeComposition = node.NativeComposition;
	out.Overlay = node.Overlay;
	out.CompositionIsolated = node.CompositionIsolated;
	out.CompositionIsolationRoot = node.CompositionIsolationRoot;
	out.TransformOnlyGeometryDirty = node.TransformOnlyGeometryDirty;
	out.SegmentIndex = node.SegmentIndex;
	if (node.SegmentIndex < _segments.size())
	{
		const auto& segment = _segments[node.SegmentIndex];
		out.CompositionIsolationDepth = segment.IsolationRoots.size();
		out.CompositionTransform = segment.LogicalTransform;
		if (node.GeometryRasterMemberIndex
			< segment.GeometryRasterMembers.size())
		{
			const auto& member = segment.GeometryRasterMembers[
				node.GeometryRasterMemberIndex];
			out.CompositionIsolationDepth = member.IsolationRoots.size();
			out.CompositionTransform = member.LogicalTransform;
		}
		out.CompositionSurfaceOriginDip =
			segment.SurfaceOriginDip;
		out.CompositionSurfacePhysicalWidth =
			segment.SurfaceProperties.PhysicalWidth;
		out.CompositionSurfacePhysicalHeight =
			segment.SurfaceProperties.PhysicalHeight;
		out.CompositionHasAncestorClip = segment.HasAncestorClip;
		out.CompositionOpacity = segment.StagedOpacity;
	}
	out.RenderedBounds = node.RenderedBounds;
	return true;
}

int PresentationScene::GetOrder(const Control* control) const noexcept
{
	if (!control) return 0;
	const auto found = std::find_if(
		_nodes.begin(), _nodes.end(),
		[control](const Node& node)
		{
			return node.Element.Get() == control;
		});
	return found == _nodes.end()
		? static_cast<int>(_nodes.size()) : found->Order;
}
