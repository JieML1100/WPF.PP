#include "Window.h"
#include "EventInfrastructure.h"
#include "InputInfrastructure.h"
#include "PresentationInfrastructure.h"
#include "Graphics.h"
#include "Button.h"
#include "CalendarView.h"
#include "CheckBox.h"
#include "ComboBox.h"
#include "ContextMenu.h"
#include "Expander.h"
#include "GroupBox.h"
#include "Label.h"
#include "ListBox.h"
#include "ListView.h"
#include "LoadingRing.h"
#include "Menu.h"
#include "NativeSurface.h"
#include "NotifyIcon.h"
#include "NumericUpDown.h"
#include "Panel.h"
#include "PasswordBox.h"
#include "Image.h"
#include "Popup.h"
#include "ProgressBar.h"
#include "ProgressRing.h"
#include "RadioButton.h"
#include "RichTextBox.h"
#include "ScrollViewer.h"
#include "Slider.h"
#include "StatusBar.h"
#include "Switch.h"
#include "TabControl.h"
#include "Taskbar.h"
#include "TextBox.h"
#include "ToolBar.h"
#include "TreeView.h"
#include "Layout/OverlayLayout.h"
#include "NotifyIcon.h"
#include "Layout/CanvasLayout.h"
#include "PlatformWindowHost.h"
#include "PresentationRenderHost.h"
#include "PresentationScene.h"
#include "Core/Threading.h"
#include <algorithm>
#include <functional>
#include <cmath>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <oleidl.h>
#include <oleacc.h>
#include <uiautomationcore.h>
#include <uiautomationcoreapi.h>
#include <uiautomationclient.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <dcomp.h>
#include <windowsx.h>

#pragma comment(lib, "Oleacc.lib")
#pragma comment(lib, "Uiautomationcore.lib")

namespace
{
	class PresentationFrameTimingClock final
	{
	public:
		PresentationFrameTimingClock() noexcept
		{
			(void)::QueryPerformanceCounter(&_start);
			_last = _start;
		}

		double LapMicroseconds() noexcept
		{
			LARGE_INTEGER now{};
			(void)::QueryPerformanceCounter(&now);
			const double result = ToMicroseconds(now.QuadPart - _last.QuadPart);
			_last = now;
			return result;
		}

		double TotalMicroseconds() const noexcept
		{
			LARGE_INTEGER now{};
			(void)::QueryPerformanceCounter(&now);
			return ToMicroseconds(now.QuadPart - _start.QuadPart);
		}

	private:
		static double ToMicroseconds(LONGLONG ticks) noexcept
		{
			static const LONGLONG frequency = []() noexcept
			{
				LARGE_INTEGER value{};
				return ::QueryPerformanceFrequency(&value) && value.QuadPart > 0
					? value.QuadPart : LONGLONG{ 1 };
			}();
			return static_cast<double>(ticks) * 1'000'000.0
				/ static_cast<double>(frequency);
		}

		LARGE_INTEGER _start{};
		LARGE_INTEGER _last{};
	};

	// WM_PAINT is deliberately the lowest-priority item in a Win32 message
	// queue.  A continuous hardware-input stream can therefore keep a valid
	// DataGrid frame waiting until the gesture stops.  The posted dispatch is
	// coalesced per Window and runs before queued hardware input; it does not
	// render itself, but asks the ordinary WM_PAINT transaction to commit once.
	UINT CuiPresentationDispatchMessage() noexcept
	{
		static const UINT message = ::RegisterWindowMessageW(
			L"CUI.Window.CoalescedPresentation.v1");
		return message;
	}

	UINT CuiAnimationFrameDispatchMessage() noexcept
	{
		static const UINT message = ::RegisterWindowMessageW(
			L"CUI.Window.AnimationFrame.v1");
		return message;
	}
}

class AnimationFrameScheduler final
{
private:
	struct State final
	{
		HWND Window = nullptr;
		UINT Message = 0;
		UINT IntervalMilliseconds = 16;
		UINT_PTR Generation = 0;
		HANDLE StopEvent = nullptr;
		std::atomic<bool> Posted = false;
		std::atomic<bool> Running = false;

		~State()
		{
			if (StopEvent) ::CloseHandle(StopEvent);
		}
	};

	std::shared_ptr<State> _state;
	std::thread _thread;

	static bool PostFrame(const std::shared_ptr<State>& state) noexcept
	{
		if (!state || state->Posted.exchange(true)) return true;
		if (state->Window && state->Message != 0
			&& ::PostMessageW(
				state->Window, state->Message,
				state->Generation, 0) != FALSE) return true;
		state->Posted.store(false);
		return false;
	}

	static void Run(const std::shared_ptr<State>& state) noexcept
	{
		if (!state || !state->StopEvent) return;
		struct RunningReset final
		{
			State* Value = nullptr;
			~RunningReset()
			{
				if (Value) Value->Running.store(false);
			}
		} runningReset{ state.get() };
		BOOL compositionEnabled = FALSE;
		bool useDwmCadence = SUCCEEDED(
			::DwmIsCompositionEnabled(&compositionEnabled))
			&& compositionEnabled != FALSE;
		ULONGLONG lastPosted = 0;
		while (useDwmCadence
			&& ::WaitForSingleObject(state->StopEvent, 0) == WAIT_TIMEOUT)
		{
			if (FAILED(::DwmFlush()))
			{
				useDwmCadence = false;
				break;
			}
			if (::WaitForSingleObject(state->StopEvent, 0) != WAIT_TIMEOUT)
				return;
			const auto now = ::GetTickCount64();
			if (lastPosted != 0
				&& state->IntervalMilliseconds > 16u
				&& now - lastPosted < state->IntervalMilliseconds)
				continue;
			lastPosted = now;
			if (!PostFrame(state)
				&& (!state->Window || ::IsWindow(state->Window) == FALSE))
				return;
		}
		if (useDwmCadence) return;
		while (::WaitForSingleObject(
			state->StopEvent,
			state->IntervalMilliseconds == 0u
				? 16u : state->IntervalMilliseconds) == WAIT_TIMEOUT)
			if (!PostFrame(state)
				&& (!state->Window || ::IsWindow(state->Window) == FALSE))
				return;
	}

	static UINT_PTR NextGeneration() noexcept
	{
		static std::atomic<UINT_PTR> next{ 1u };
		auto value = next.fetch_add(1u);
		if (value != 0u) return value;
		value = next.fetch_add(1u);
		return value == 0u ? 1u : value;
	}

public:
	class DispatchLease final
	{
	private:
		std::shared_ptr<State> _state;
		UINT_PTR _generation = 0;

		explicit DispatchLease(
			std::shared_ptr<State> state,
			UINT_PTR generation) noexcept
			: _state(std::move(state)), _generation(generation)
		{
		}

		void Release() noexcept
		{
			if (_state && _state->Generation == _generation)
				_state->Posted.store(false);
			_state.reset();
			_generation = 0;
		}

		friend class AnimationFrameScheduler;

	public:
		DispatchLease() = default;
		DispatchLease(const DispatchLease&) = delete;
		DispatchLease& operator=(const DispatchLease&) = delete;

		DispatchLease(DispatchLease&& other) noexcept
			: _state(std::move(other._state)),
			_generation(std::exchange(other._generation, 0))
		{
		}

		DispatchLease& operator=(DispatchLease&& other) noexcept
		{
			if (this == &other) return *this;
			Release();
			_state = std::move(other._state);
			_generation = std::exchange(other._generation, 0);
			return *this;
		}

		~DispatchLease() { Release(); }

		explicit operator bool() const noexcept
		{
			return _state != nullptr;
		}
	};

	~AnimationFrameScheduler() { Stop(); }

	bool Start(HWND window, UINT message, UINT intervalMilliseconds) noexcept
	{
		if (_state && _thread.joinable() && _state->Running.load()
			&& _state->Window == window && _state->Message == message
			&& _state->IntervalMilliseconds == intervalMilliseconds)
			return true;
		Stop();
		try
		{
			auto state = std::make_shared<State>();
			state->Window = window;
			state->Message = message;
			state->IntervalMilliseconds = intervalMilliseconds == 0u
				? 16u : intervalMilliseconds;
			state->Generation = NextGeneration();
			state->StopEvent = ::CreateEventW(
				nullptr, TRUE, FALSE, nullptr);
			if (!state->StopEvent) return false;
			state->Running.store(true);
			std::thread worker([state] { Run(state); });
			_state = std::move(state);
			_thread = std::move(worker);
			return true;
		}
		catch (...)
		{
			Stop();
			return false;
		}
	}

	DispatchLease BeginDispatch(UINT_PTR generation) noexcept
	{
		if (!_state || _state->Generation != generation
			|| !_state->Posted.load()) return {};
		return DispatchLease(_state, generation);
	}

	bool IsRunning() const noexcept
	{
		return _state && _thread.joinable() && _state->Running.load();
	}

	bool IsFramePosted() const noexcept
	{
		return _state && _state->Posted.load();
	}

	void Stop() noexcept
	{
		auto state = std::move(_state);
		if (state && state->StopEvent) ::SetEvent(state->StopEvent);
		if (_thread.joinable()) _thread.join();
	}
};

namespace accessibility_detail
{
	struct SafeArrayDestroyer
	{
		void operator()(SAFEARRAY* value) const noexcept
		{
			if (value) (void)::SafeArrayDestroy(value);
		}
	};

	using UniqueSafeArray = std::unique_ptr<SAFEARRAY, SafeArrayDestroyer>;

	LONG ToMsaaRole(AutomationControlType controlType)
	{
		switch (controlType)
		{
		case AutomationControlType::Window: return ROLE_SYSTEM_WINDOW;
		case AutomationControlType::Pane: return ROLE_SYSTEM_PANE;
		case AutomationControlType::Group: return ROLE_SYSTEM_GROUPING;
		case AutomationControlType::Text: return ROLE_SYSTEM_STATICTEXT;
		case AutomationControlType::Hyperlink: return ROLE_SYSTEM_LINK;
		case AutomationControlType::Button: return ROLE_SYSTEM_PUSHBUTTON;
		case AutomationControlType::CheckBox: return ROLE_SYSTEM_CHECKBUTTON;
		case AutomationControlType::RadioButton: return ROLE_SYSTEM_RADIOBUTTON;
		case AutomationControlType::Edit: return ROLE_SYSTEM_TEXT;
		case AutomationControlType::ComboBox: return ROLE_SYSTEM_COMBOBOX;
		case AutomationControlType::Spinner: return ROLE_SYSTEM_SPINBUTTON;
		case AutomationControlType::List: return ROLE_SYSTEM_LIST;
		case AutomationControlType::ListItem: return ROLE_SYSTEM_LISTITEM;
		case AutomationControlType::DataGrid: return ROLE_SYSTEM_TABLE;
		case AutomationControlType::Tree: return ROLE_SYSTEM_OUTLINE;
		case AutomationControlType::TreeItem: return ROLE_SYSTEM_OUTLINEITEM;
		case AutomationControlType::DataItem: return ROLE_SYSTEM_CELL;
		case AutomationControlType::HeaderItem: return ROLE_SYSTEM_COLUMNHEADER;
		case AutomationControlType::Tab: return ROLE_SYSTEM_PAGETABLIST;
		case AutomationControlType::TabItem: return ROLE_SYSTEM_PAGETAB;
		case AutomationControlType::Menu: return ROLE_SYSTEM_MENUPOPUP;
		case AutomationControlType::MenuItem: return ROLE_SYSTEM_MENUITEM;
		case AutomationControlType::ToolBar: return ROLE_SYSTEM_TOOLBAR;
		case AutomationControlType::StatusBar: return ROLE_SYSTEM_STATUSBAR;
		case AutomationControlType::Slider: return ROLE_SYSTEM_SLIDER;
		case AutomationControlType::ProgressBar: return ROLE_SYSTEM_PROGRESSBAR;
		case AutomationControlType::ScrollBar: return ROLE_SYSTEM_SCROLLBAR;
		case AutomationControlType::Image: return ROLE_SYSTEM_GRAPHIC;
		case AutomationControlType::Document: return ROLE_SYSTEM_DOCUMENT;
		case AutomationControlType::Separator: return ROLE_SYSTEM_SEPARATOR;
		case AutomationControlType::Calendar: return ROLE_SYSTEM_CLIENT;
		default: return ROLE_SYSTEM_CLIENT;
		}
	}

	HRESULT CopyBstr(const std::wstring& value, BSTR* result)
	{
		if (!result) return E_POINTER;
		*result = nullptr;
		if (value.empty()) return S_FALSE;
		*result = ::SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
		return *result ? S_OK : E_OUTOFMEMORY;
	}

	std::wstring DefaultActionName(AutomationControlType controlType)
	{
		switch (controlType)
		{
		case AutomationControlType::Hyperlink: return L"Open";
		case AutomationControlType::CheckBox: return L"Check";
		case AutomationControlType::RadioButton: return L"Select";
		case AutomationControlType::MenuItem: return L"Invoke";
		default: return L"Press";
		}
	}

	HRESULT SetVariantString(VARIANT* value, const std::wstring& text)
	{
		if (!value) return E_POINTER;
		::VariantInit(value);
		value->vt = VT_BSTR;
		value->bstrVal = ::SysAllocStringLen(
			text.data(), static_cast<UINT>(text.size()));
		return value->bstrVal ? S_OK : E_OUTOFMEMORY;
	}

	void SetVariantBool(VARIANT* value, bool state)
	{
		::VariantInit(value);
		value->vt = VT_BOOL;
		value->boolVal = state ? VARIANT_TRUE : VARIANT_FALSE;
	}

	void SetVariantInt(VARIANT* value, int number)
	{
		::VariantInit(value);
		value->vt = VT_I4;
		value->lVal = number;
	}

	// Pattern property getters return ownership of their provider references or
	// SAFEARRAYs to the caller.  Transfer that ownership into the VARIANT so a
	// later VariantClear performs the matching Release/SafeArrayDestroy.
	HRESULT TakeVariantProvider(
		VARIANT* value, IRawElementProviderSimple* provider)
	{
		if (!value)
		{
			if (provider) provider->Release();
			return E_POINTER;
		}
		::VariantInit(value);
		if (!provider) return S_OK;
		value->vt = VT_UNKNOWN;
		value->punkVal = static_cast<IUnknown*>(provider);
		return S_OK;
	}

	HRESULT TakeVariantProviderArray(VARIANT* value, SAFEARRAY* providers)
	{
		if (!value)
		{
			if (providers) (void)::SafeArrayDestroy(providers);
			return E_POINTER;
		}
		::VariantInit(value);
		if (!providers) return S_OK;
		value->vt = static_cast<VARTYPE>(VT_ARRAY | VT_UNKNOWN);
		value->parray = providers;
		return S_OK;
	}

	HRESULT ToHresult(AutomationOperationResult result)
	{
		switch (result)
		{
		case AutomationOperationResult::Succeeded: return S_OK;
		case AutomationOperationResult::NotSupported: return UIA_E_NOTSUPPORTED;
		case AutomationOperationResult::InvalidOperation:
			return UIA_E_INVALIDOPERATION;
		case AutomationOperationResult::ElementNotEnabled:
			return UIA_E_ELEMENTNOTENABLED;
		case AutomationOperationResult::InvalidArgument: return E_INVALIDARG;
		case AutomationOperationResult::ElementNotAvailable:
			return UIA_E_ELEMENTNOTAVAILABLE;
		default: return UIA_E_INVALIDOPERATION;
		}
	}

	int ToUiaControlType(AutomationControlType controlType)
	{
		switch (controlType)
		{
		case AutomationControlType::Window: return UIA_WindowControlTypeId;
		case AutomationControlType::Pane: return UIA_PaneControlTypeId;
		case AutomationControlType::Group: return UIA_GroupControlTypeId;
		case AutomationControlType::Text: return UIA_TextControlTypeId;
		case AutomationControlType::Hyperlink: return UIA_HyperlinkControlTypeId;
		case AutomationControlType::Button: return UIA_ButtonControlTypeId;
		case AutomationControlType::CheckBox: return UIA_CheckBoxControlTypeId;
		case AutomationControlType::RadioButton: return UIA_RadioButtonControlTypeId;
		case AutomationControlType::Edit: return UIA_EditControlTypeId;
		case AutomationControlType::ComboBox: return UIA_ComboBoxControlTypeId;
		case AutomationControlType::Spinner: return UIA_SpinnerControlTypeId;
		case AutomationControlType::List: return UIA_ListControlTypeId;
		case AutomationControlType::ListItem: return UIA_ListItemControlTypeId;
		case AutomationControlType::DataGrid: return UIA_DataGridControlTypeId;
		case AutomationControlType::Tree: return UIA_TreeControlTypeId;
		case AutomationControlType::TreeItem: return UIA_TreeItemControlTypeId;
		case AutomationControlType::DataItem: return UIA_DataItemControlTypeId;
		case AutomationControlType::HeaderItem: return UIA_HeaderItemControlTypeId;
		case AutomationControlType::Tab: return UIA_TabControlTypeId;
		case AutomationControlType::TabItem: return UIA_TabItemControlTypeId;
		case AutomationControlType::Menu: return UIA_MenuControlTypeId;
		case AutomationControlType::MenuItem: return UIA_MenuItemControlTypeId;
		case AutomationControlType::ToolBar: return UIA_ToolBarControlTypeId;
		case AutomationControlType::StatusBar: return UIA_StatusBarControlTypeId;
		case AutomationControlType::Slider: return UIA_SliderControlTypeId;
		case AutomationControlType::ProgressBar: return UIA_ProgressBarControlTypeId;
		case AutomationControlType::ScrollBar: return UIA_ScrollBarControlTypeId;
		case AutomationControlType::Image: return UIA_ImageControlTypeId;
		case AutomationControlType::Document: return UIA_DocumentControlTypeId;
		case AutomationControlType::Separator: return UIA_SeparatorControlTypeId;
		case AutomationControlType::Calendar: return UIA_CalendarControlTypeId;
		default: return UIA_CustomControlTypeId;
		}
	}

}

class WindowAccessibleObject final : public IAccessible
{
public:
	explicit WindowAccessibleObject(Window* form) : _form(form) {}

	void DetachWindow() noexcept { _form = nullptr; }

	long ChildIdFor(Control* control) const
	{
		try
		{
			auto* form = _form;
			if (!form || !control) return CHILDID_SELF;
			const ControlWeakReference formLifetime(form);
			const ControlWeakReference controlLifetime(control);
			auto controls = form->GetAccessibleControls();
			form = dynamic_cast<Window*>(formLifetime.Get());
			control = controlLifetime.Get();
			if (!form || _form != form || !control) return CHILDID_SELF;
			auto* representative = form->ResolveAccessibleRepresentative(
				control, controls);
			form = dynamic_cast<Window*>(formLifetime.Get());
			if (!form || _form != form) return CHILDID_SELF;
			auto position = std::find(
				controls.begin(), controls.end(), representative);
			return position == controls.end()
				? CHILDID_SELF
				: static_cast<long>(position - controls.begin()) + 1;
		}
		catch (...)
		{
			// IAccessible methods must never allow a C++ exception to cross COM.
			return CHILDID_SELF;
		}
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
	{
		if (!object) return E_POINTER;
		*object = nullptr;
		if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IAccessible)
			*object = static_cast<IAccessible*>(this);
		else
			return E_NOINTERFACE;
		AddRef();
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++_references;
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG remaining = --_references;
		if (remaining == 0) delete this;
		return remaining;
	}

	HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override
	{
		if (!count) return E_POINTER;
		*count = 0;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD,
		DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }

	HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override
	{
		if (!parent) return E_POINTER;
		*parent = nullptr;
		return Connected() ? S_FALSE : CO_E_OBJNOTCONNECTED;
	}

	HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override
	{
		if (!count) return E_POINTER;
		if (!Connected()) return CO_E_OBJNOTCONNECTED;
		const auto controls = _form->GetAccessibleControls();
		*count = controls.size() > static_cast<size_t>(LONG_MAX)
			? LONG_MAX : static_cast<long>(controls.size());
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch** dispatch) override
	{
		if (!dispatch) return E_POINTER;
		*dispatch = nullptr;
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		return S_FALSE; // Children are simple MSAA elements addressed by child id.
	}

	HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override
	{
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		return accessibility_detail::CopyBstr(
			self ? static_cast<std::wstring>(_form->Title)
				: control->GetAccessibilitySnapshot().Name, name);
	}

	HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override
	{
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		return accessibility_detail::CopyBstr(
			self ? std::wstring{} : control->GetAccessibilitySnapshot().Value, value);
	}

	HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR* description) override
	{
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		return accessibility_detail::CopyBstr(self ? std::wstring{}
			: control->GetAccessibilitySnapshot().Description, description);
	}

	HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override
	{
		if (!role) return E_POINTER;
		::VariantInit(role);
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		role->vt = VT_I4;
		role->lVal = self ? ROLE_SYSTEM_CLIENT
			: accessibility_detail::ToMsaaRole(
				control->GetAccessibilitySnapshot().ControlType);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override
	{
		if (!state) return E_POINTER;
		::VariantInit(state);
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		LONG flags = 0;
		if (self)
		{
			if (!::IsWindowEnabled(_form->Handle)) flags |= STATE_SYSTEM_UNAVAILABLE;
			if (!::IsWindowVisible(_form->Handle)) flags |= STATE_SYSTEM_INVISIBLE;
			if (::GetFocus() == _form->Handle) flags |= STATE_SYSTEM_FOCUSED;
		}
		else
		{
			const auto snapshot = control->GetAccessibilitySnapshot();
			if (!snapshot.Enabled) flags |= STATE_SYSTEM_UNAVAILABLE;
			if (!snapshot.Visible || !::IsWindowVisible(_form->Handle))
				flags |= STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN;
			if (snapshot.Focusable) flags |= STATE_SYSTEM_FOCUSABLE;
			if (snapshot.Focused) flags |= STATE_SYSTEM_FOCUSED;
			if (snapshot.Selected) flags |= STATE_SYSTEM_SELECTED;
			if (snapshot.Checked) flags |= STATE_SYSTEM_CHECKED;
			if (snapshot.Password) flags |= STATE_SYSTEM_PROTECTED;
			if (snapshot.ReadOnly) flags |= STATE_SYSTEM_READONLY;
			if (snapshot.ControlType == AutomationControlType::ComboBox)
				flags |= STATE_SYSTEM_HASPOPUP;
		}
		state->vt = VT_I4;
		state->lVal = flags;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT child, BSTR* help) override
	{
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		return accessibility_detail::CopyBstr(self ? std::wstring{}
			: control->GetAccessibilitySnapshot().HelpText, help);
	}

	HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* helpFile, VARIANT, long* topic) override
	{
		if (!helpFile || !topic) return E_POINTER;
		*helpFile = nullptr;
		*topic = 0;
		return Connected() ? S_FALSE : CO_E_OBJNOTCONNECTED;
	}

	HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(
		VARIANT child, BSTR* shortcut) override
	{
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		return accessibility_detail::CopyBstr(self ? std::wstring{}
			: control->GetAccessibilitySnapshot().KeyboardShortcut, shortcut);
	}

	HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* child) override
	{
		if (!child) return E_POINTER;
		::VariantInit(child);
		try
		{
			auto* form = _form;
			if (!form || !Connected()) return CO_E_OBJNOTCONNECTED;
			const ControlWeakReference formLifetime(form);
			if (::GetFocus() != form->Handle) return S_FALSE;
			const long id = ChildIdFor(form->GetKeyboardFocusedElement());
			form = dynamic_cast<Window*>(formLifetime.Get());
			if (!form || _form != form || !Connected())
				return CO_E_OBJNOTCONNECTED;
			if (::GetFocus() != form->Handle) return S_FALSE;
			child->vt = VT_I4;
			child->lVal = id;
			return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return E_FAIL; }
	}

	HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* children) override
	{
		if (!children) return E_POINTER;
		::VariantInit(children);
		if (!Connected()) return CO_E_OBJNOTCONNECTED;
		const long id = ChildIdFor(_form->GetKeyboardFocusedElement());
		if (id == CHILDID_SELF) return S_FALSE;
		children->vt = VT_I4;
		children->lVal = id;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override
	{
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		if (self) return accessibility_detail::CopyBstr({}, action);
		auto& peer = control->GetAutomationPeer();
		if (!peer.SupportsPattern(AutomationPattern::Invoke)
			&& !peer.SupportsPattern(AutomationPattern::Toggle)
			&& !peer.SupportsPattern(AutomationPattern::SelectionItem))
			return accessibility_detail::CopyBstr({}, action);
		return accessibility_detail::CopyBstr(
			accessibility_detail::DefaultActionName(
				peer.GetAutomationControlType()), action);
	}

	HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override
	{
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		if (self)
		{
			if ((flags & SELFLAG_TAKEFOCUS) && _form->Handle)
				::SetFocus(_form->Handle);
			return S_OK;
		}
		if (flags & SELFLAG_REMOVESELECTION)
		{
			if (_form->GetKeyboardFocusedElement() == control) _form->SetKeyboardFocus(nullptr, true);
			return S_OK;
		}
		if (flags & (SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION | SELFLAG_ADDSELECTION))
			return control->Focus() ? S_OK : S_FALSE;
		return S_FALSE;
	}

	HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top, long* width,
		long* height, VARIANT child) override
	{
		if (!left || !top || !width || !height) return E_POINTER;
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		RECT rectangle{};
		if (self)
		{
			if (!::GetClientRect(_form->Handle, &rectangle)) return HRESULT_FROM_WIN32(::GetLastError());
		}
		else
		{
			rectangle = _form->ContentDipRectToClientPixels(
				control->GetRenderedAbsoluteRectDip());
		}
		POINT points[2]{ { rectangle.left, rectangle.top }, { rectangle.right, rectangle.bottom } };
		::MapWindowPoints(_form->Handle, nullptr, points, 2);
		*left = points[0].x;
		*top = points[0].y;
		*width = points[1].x - points[0].x;
		*height = points[1].y - points[0].y;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT start,
		VARIANT* destination) override
	{
		if (!destination) return E_POINTER;
		::VariantInit(destination);
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(start, self, control);
		if (FAILED(resolved)) return resolved;
		auto controls = _form->GetAccessibleControls();
		long id = self ? CHILDID_SELF : ChildIdFor(control);
		if (self && direction == NAVDIR_FIRSTCHILD && !controls.empty()) id = 1;
		else if (self && direction == NAVDIR_LASTCHILD && !controls.empty())
			id = static_cast<long>(controls.size());
		else if (!self && (direction == NAVDIR_NEXT || direction == NAVDIR_DOWN || direction == NAVDIR_RIGHT)
			&& id < static_cast<long>(controls.size())) ++id;
		else if (!self && (direction == NAVDIR_PREVIOUS || direction == NAVDIR_UP || direction == NAVDIR_LEFT)
			&& id > 1) --id;
		else return S_FALSE;
		destination->vt = VT_I4;
		destination->lVal = id;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT* child) override
	{
		if (!child) return E_POINTER;
		::VariantInit(child);
		if (!Connected()) return CO_E_OBJNOTCONNECTED;
		auto controls = _form->GetAccessibleControls();
		for (size_t index = controls.size(); index > 0; --index)
		{
			Control* control = controls[index - 1];
			if (!control) continue;
			VARIANT id{};
			id.vt = VT_I4;
			id.lVal = static_cast<long>(index);
			long left = 0, top = 0, width = 0, height = 0;
			if (SUCCEEDED(accLocation(&left, &top, &width, &height, id))
				&& x >= left && y >= top && x < left + width && y < top + height)
			{
				child->vt = VT_I4;
				child->lVal = static_cast<long>(index);
				return S_OK;
			}
		}
		VARIANT self{};
		self.vt = VT_I4;
		self.lVal = CHILDID_SELF;
		long left = 0, top = 0, width = 0, height = 0;
		if (SUCCEEDED(accLocation(&left, &top, &width, &height, self))
			&& x >= left && y >= top && x < left + width && y < top + height)
		{
			child->vt = VT_I4;
			child->lVal = CHILDID_SELF;
			return S_OK;
		}
		return S_FALSE;
	}

	HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override
	{
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		if (self) return S_FALSE;
		auto& peer = control->GetAutomationPeer();
		if (!peer.SupportsPattern(AutomationPattern::Invoke)
			&& !peer.SupportsPattern(AutomationPattern::Toggle)
			&& !peer.SupportsPattern(AutomationPattern::SelectionItem))
			return S_FALSE;
		const auto result = peer.SupportsPattern(AutomationPattern::Invoke)
			? peer.Invoke()
			: (peer.SupportsPattern(AutomationPattern::Toggle)
				? peer.Toggle() : peer.Select());
		return result == AutomationOperationResult::Succeeded ? S_OK : S_FALSE;
	}

	HRESULT STDMETHODCALLTYPE put_accName(VARIANT child, BSTR name) override
	{
		bool self = false;
		Control* control = nullptr;
		const HRESULT resolved = Resolve(child, self, control);
		if (FAILED(resolved)) return resolved;
		if (self) return E_NOTIMPL;
		control->AutomationName = name ? name : L"";
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override
	{
		return Connected() ? E_NOTIMPL : CO_E_OBJNOTCONNECTED;
	}

private:
	bool Connected() const noexcept
	{
		return _form && _form->Handle && ::IsWindow(_form->Handle);
	}

	HRESULT Resolve(VARIANT child, bool& self, Control*& control) const
	{
		self = false;
		control = nullptr;
		if (!Connected()) return CO_E_OBJNOTCONNECTED;
		if (child.vt != VT_I4) return E_INVALIDARG;
		if (child.lVal == CHILDID_SELF)
		{
			self = true;
			return S_OK;
		}
		auto controls = _form->GetAccessibleControls();
		if (child.lVal < 1 || static_cast<size_t>(child.lVal) > controls.size())
			return E_INVALIDARG;
		control = controls[static_cast<size_t>(child.lVal - 1)];
		return control ? S_OK : E_INVALIDARG;
	}

	std::atomic<ULONG> _references{ 1 };
	Window* _form = nullptr;
};

class ControlUiaProvider;
class VirtualUiaProvider;

class WindowUiaProvider final :
	public IRawElementProviderSimple,
	public IRawElementProviderFragment,
	public IRawElementProviderFragmentRoot
{
public:
	explicit WindowUiaProvider(Window* form) : _form(form) {}

	void DetachWindow() noexcept { _form.store(nullptr, std::memory_order_release); }
	bool Connected() const noexcept
	{
		auto* form = _form.load(std::memory_order_acquire);
		return form && form->Handle && ::IsWindow(form->Handle);
	}
	bool HasNativeKeyboardFocus() const noexcept
	{
		auto* form = _form.load(std::memory_order_acquire);
		return form && form->Handle && ::IsWindow(form->Handle)
			&& ::GetFocus() == form->Handle;
	}
	Window* GetWindow() const noexcept
	{
		return _form.load(std::memory_order_acquire);
	}
	Control* ResolveControl(Control* candidate) const noexcept;
	Control* ResolveControl(
		Control* candidate, uint32_t runtimeId) const noexcept;
	ControlUiaProvider* ProviderFor(Control* control);
	VirtualUiaProvider* VirtualProviderFor(Control* owner, uint32_t virtualId);
	VirtualUiaProvider* VirtualProviderForValidated(
		Control* owner, uint32_t ownerRuntimeId, uint32_t virtualId);
	void UnregisterProvider(uint32_t runtimeId, ControlUiaProvider* provider);
	void UnregisterVirtualProvider(
		uint64_t key, VirtualUiaProvider* provider);
	std::vector<Control*> DirectChildren(Control* parent) const;
	bool TryGetVirtualBoundaryChild(Control* owner, uint32_t parentId,
		bool last, uint32_t& result) const;
	bool TryGetVirtualSibling(Control* owner, uint32_t parentId,
		uint32_t id, bool next, uint32_t& result) const;
	bool ResolveVirtualNode(Control* owner, uint32_t ownerRuntimeId,
		uint32_t virtualId, AccessibilityVirtualNode& node,
		AutomationPeer** source = nullptr,
		std::shared_ptr<AutomationPeer>* sourceLease = nullptr) const;
	Control* ParentOf(Control* control) const;
	Control* SiblingOf(Control* control, bool next) const;
	void RaiseEvent(Control* control, AccessibilityChange change);
	void RaiseVirtualEvent(
		Control* owner, uint32_t virtualId, AccessibilityChange change);
	void SetVirtualFocus(Control* owner, uint32_t virtualId);
	void ClearVirtualFocus() noexcept;
	bool IsVirtualFocused(Control* owner, uint32_t virtualId);
	uint32_t VirtualFocusFor(Control* owner);
	bool TryGetVirtualFocusedNode(
		Control* focused, Control*& owner, uint32_t& virtualId);

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
	{
		if (!object) return E_POINTER;
		*object = nullptr;
		try
		{
		if (iid == IID_IUnknown || iid == IID_IRawElementProviderSimple)
			*object = static_cast<IRawElementProviderSimple*>(this);
		else if (iid == IID_IRawElementProviderFragment)
			*object = static_cast<IRawElementProviderFragment*>(this);
		else if (iid == IID_IRawElementProviderFragmentRoot)
			*object = static_cast<IRawElementProviderFragmentRoot*>(this);
		else
			return E_NOINTERFACE;
		AddRef();
		return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++_references; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG remaining = --_references;
		if (remaining == 0) delete this;
		return remaining;
	}

	HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* value) override
	{
		if (!value) return E_POINTER;
		*value = ProviderOptions_ServerSideProvider;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		return Connected() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}
	HRESULT STDMETHODCALLTYPE GetPropertyValue(
		PROPERTYID propertyId, VARIANT* value) override try
	{
		if (!value) return E_POINTER;
		::VariantInit(value);
		auto* form = GetWindow();
		if (!form || !Connected()) return UIA_E_ELEMENTNOTAVAILABLE;
		switch (propertyId)
		{
		case UIA_NamePropertyId:
			return accessibility_detail::SetVariantString(value,
				static_cast<std::wstring>(form->Title));
		case UIA_ControlTypePropertyId:
			accessibility_detail::SetVariantInt(value, UIA_WindowControlTypeId);
			return S_OK;
		case UIA_ClassNamePropertyId:
			return accessibility_detail::SetVariantString(value, L"CUI.Window");
		case UIA_FrameworkIdPropertyId:
			return accessibility_detail::SetVariantString(value, L"CUI");
		case UIA_ProviderDescriptionPropertyId:
			return accessibility_detail::SetVariantString(
				value, L"CUI native UI Automation provider");
		case UIA_ProcessIdPropertyId:
			accessibility_detail::SetVariantInt(
				value, static_cast<int>(::GetCurrentProcessId()));
			return S_OK;
		case UIA_NativeWindowHandlePropertyId:
			accessibility_detail::SetVariantInt(
				value, static_cast<int>(reinterpret_cast<INT_PTR>(form->Handle)));
			return S_OK;
		case UIA_IsEnabledPropertyId:
			accessibility_detail::SetVariantBool(value, form->IsEnabled);
			return S_OK;
		case UIA_IsKeyboardFocusablePropertyId:
			accessibility_detail::SetVariantBool(value, true);
			return S_OK;
		case UIA_HasKeyboardFocusPropertyId:
			accessibility_detail::SetVariantBool(value,
				HasNativeKeyboardFocus()
				&& form->GetKeyboardFocusedElement() == nullptr);
			return S_OK;
		case UIA_IsOffscreenPropertyId:
			accessibility_detail::SetVariantBool(
				value, !form->IsVisible || !::IsWindowVisible(form->Handle));
			return S_OK;
		case UIA_IsControlElementPropertyId:
		case UIA_IsContentElementPropertyId:
			accessibility_detail::SetVariantBool(value, true);
			return S_OK;
		default:
			return S_OK;
		}
	}
	catch (const std::bad_alloc&)
	{
		if (value) { (void)::VariantClear(value); ::VariantInit(value); }
		return E_OUTOFMEMORY;
	}
	catch (...)
	{
		if (value) { (void)::VariantClear(value); ::VariantInit(value); }
		return UIA_E_INVALIDOPERATION;
	}
	HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
		IRawElementProviderSimple** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		auto* form = GetWindow();
		if (!form || !Connected()) return UIA_E_ELEMENTNOTAVAILABLE;
		return ::UiaHostProviderFromHwnd(form->Handle, value);
	}

	HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
		IRawElementProviderFragment** value) override;
	HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		return Connected() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}
	HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* value) override try
	{
		if (!value) return E_POINTER;
		*value = UiaRect{};
		auto* form = GetWindow();
		if (!form || !Connected()) return UIA_E_ELEMENTNOTAVAILABLE;
		RECT rectangle{};
		if (!::GetClientRect(form->Handle, &rectangle))
			return HRESULT_FROM_WIN32(::GetLastError());
		POINT points[2]{ { rectangle.left, rectangle.top },
			{ rectangle.right, rectangle.bottom } };
		::MapWindowPoints(form->Handle, nullptr, points, 2);
		value->left = points[0].x;
		value->top = points[0].y;
		value->width = points[1].x - points[0].x;
		value->height = points[1].y - points[0].y;
		return S_OK;
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		return Connected() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}
	HRESULT STDMETHODCALLTYPE SetFocus() override
	{
		auto* form = GetWindow();
		if (!form || !Connected()) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference formLifetime(form);
		const HWND handle = form->Handle;
		(void)::SetFocus(handle);
		form = dynamic_cast<Window*>(formLifetime.Get());
		if (!form || GetWindow() != form || !Connected())
			return UIA_E_ELEMENTNOTAVAILABLE;
		return ::GetFocus() == handle
			? S_OK : UIA_E_INVALIDOPERATION;
	}
	HRESULT STDMETHODCALLTYPE get_FragmentRoot(
		IRawElementProviderFragmentRoot** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		if (!Connected()) return UIA_E_ELEMENTNOTAVAILABLE;
		*value = static_cast<IRawElementProviderFragmentRoot*>(this);
		AddRef();
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y,
		IRawElementProviderFragment** value) override;
	HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** value) override;

private:
	~WindowUiaProvider() = default;
	std::atomic<ULONG> _references{ 1 };
	std::atomic<Window*> _form{ nullptr };
	std::mutex _providerMutex;
	std::unordered_map<uint32_t, ControlUiaProvider*> _providers;
	std::unordered_map<uint64_t, VirtualUiaProvider*> _virtualProviders;
	std::unordered_map<uint32_t, uint32_t> _focusedVirtualByOwner;
};

class ControlUiaProvider final :
	public IRawElementProviderSimple,
	public IRawElementProviderFragment,
	public IInvokeProvider,
	public IToggleProvider,
	public IValueProvider,
	public IRangeValueProvider,
	public IExpandCollapseProvider,
	public ISelectionItemProvider,
	public ISelectionProvider,
	public IGridProvider,
	public ITableProvider,
	public IScrollProvider
{
public:
	ControlUiaProvider(WindowUiaProvider* root, Control* control) :
		_root(root), _control(control),
		_runtimeId(control ? control->GetAccessibilityRuntimeId() : 0)
	{
		if (_root) _root->AddRef();
	}

	uint32_t RuntimeId() const noexcept { return _runtimeId; }
	int GetAccessibilityTypeForEvent() const noexcept
	{
		try { return SupportsRange() ? 1 : 0; }
		catch (...) { return 0; }
	}
	bool SupportsToggleForEvent() const noexcept
	{
		try { return SupportsToggle(); }
		catch (...) { return false; }
	}
	bool SupportsSelectionItemForEvent() const noexcept
	{
		try { return SupportsSelectionItem(); }
		catch (...) { return false; }
	}
	bool Matches(Control* control) const noexcept
	{
		return control == _control && control
			&& control->GetAccessibilityRuntimeId() == _runtimeId;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
	{
		if (!object) return E_POINTER;
		*object = nullptr;
		try
		{
		if (iid == IID_IUnknown || iid == IID_IRawElementProviderSimple)
			*object = static_cast<IRawElementProviderSimple*>(this);
		else if (iid == IID_IRawElementProviderFragment)
			*object = static_cast<IRawElementProviderFragment*>(this);
		else if (iid == IID_IInvokeProvider && SupportsInvoke())
			*object = static_cast<IInvokeProvider*>(this);
		else if (iid == IID_IToggleProvider && SupportsToggle())
			*object = static_cast<IToggleProvider*>(this);
		else if (iid == IID_IValueProvider && SupportsValue())
			*object = static_cast<IValueProvider*>(this);
		else if (iid == IID_IRangeValueProvider && SupportsRange())
			*object = static_cast<IRangeValueProvider*>(this);
		else if (iid == IID_IExpandCollapseProvider && SupportsExpandCollapse())
			*object = static_cast<IExpandCollapseProvider*>(this);
		else if (iid == IID_ISelectionItemProvider && SupportsSelectionItem())
			*object = static_cast<ISelectionItemProvider*>(this);
		else if (iid == IID_ISelectionProvider && SupportsSelection())
			*object = static_cast<ISelectionProvider*>(this);
		else if (iid == IID_IGridProvider && SupportsGrid())
			*object = static_cast<IGridProvider*>(this);
		else if (iid == IID_ITableProvider && SupportsTable())
			*object = static_cast<ITableProvider*>(this);
		else if (iid == IID_IScrollProvider && SupportsScroll())
			*object = static_cast<IScrollProvider*>(this);
		else
			return E_NOINTERFACE;
		AddRef();
		return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++_references; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG remaining = --_references;
		if (remaining == 0) delete this;
		return remaining;
	}

	HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* value) override
	{
		if (!value) return E_POINTER;
		*value = ProviderOptions_ServerSideProvider;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId,
		IUnknown** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		try
		{
		if (!CurrentControl()) return UIA_E_ELEMENTNOTAVAILABLE;
		auto queryPattern = [this, value](REFIID iid)
		{
			const HRESULT result = QueryInterface(
				iid, reinterpret_cast<void**>(value));
			return result == E_NOINTERFACE ? S_OK : result;
		};
		if (patternId == UIA_InvokePatternId)
			return queryPattern(IID_IInvokeProvider);
		if (patternId == UIA_TogglePatternId)
			return queryPattern(IID_IToggleProvider);
		if (patternId == UIA_ValuePatternId)
			return queryPattern(IID_IValueProvider);
		if (patternId == UIA_RangeValuePatternId)
			return queryPattern(IID_IRangeValueProvider);
		if (patternId == UIA_ExpandCollapsePatternId)
			return queryPattern(IID_IExpandCollapseProvider);
		if (patternId == UIA_SelectionItemPatternId)
			return queryPattern(IID_ISelectionItemProvider);
		if (patternId == UIA_SelectionPatternId)
			return queryPattern(IID_ISelectionProvider);
		if (patternId == UIA_GridPatternId)
			return queryPattern(IID_IGridProvider);
		if (patternId == UIA_TablePatternId)
			return queryPattern(IID_ITableProvider);
		if (patternId == UIA_ScrollPatternId)
			return queryPattern(IID_IScrollProvider);
		return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId,
		VARIANT* value) override try
	{
		if (!value) return E_POINTER;
		::VariantInit(value);
		auto* control = CurrentControl();
		if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference controlLifetime(control);
		const auto snapshot = control->GetAccessibilitySnapshot();
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		switch (propertyId)
		{
		case UIA_NamePropertyId:
			return accessibility_detail::SetVariantString(value, snapshot.Name);
		case UIA_AutomationIdPropertyId:
			return accessibility_detail::SetVariantString(value, snapshot.AutomationId);
		case UIA_ControlTypePropertyId:
			accessibility_detail::SetVariantInt(value,
				accessibility_detail::ToUiaControlType(snapshot.ControlType));
			return S_OK;
		case UIA_ClassNamePropertyId:
			{
				std::wstring className;
				const HRESULT result = PerformPeerQuery(
					[&className](AutomationPeer& peer) -> HRESULT
					{
						className = peer.GetAutomationClassName();
						return S_OK;
					});
				if (FAILED(result)) return result;
				return accessibility_detail::SetVariantString(
					value, className);
			}
		case UIA_FrameworkIdPropertyId:
			return accessibility_detail::SetVariantString(value, L"CUI");
		case UIA_ProviderDescriptionPropertyId:
			return accessibility_detail::SetVariantString(
				value, L"CUI native control provider");
		case UIA_HelpTextPropertyId:
			return accessibility_detail::SetVariantString(value, snapshot.HelpText);
		case UIA_FullDescriptionPropertyId:
			return accessibility_detail::SetVariantString(value, snapshot.Description);
		case UIA_AcceleratorKeyPropertyId:
		case UIA_AccessKeyPropertyId:
			return accessibility_detail::SetVariantString(
				value, snapshot.KeyboardShortcut);
		case UIA_ProcessIdPropertyId:
			accessibility_detail::SetVariantInt(
				value, static_cast<int>(::GetCurrentProcessId()));
			return S_OK;
		case UIA_IsEnabledPropertyId:
			accessibility_detail::SetVariantBool(value, snapshot.Enabled);
			return S_OK;
		case UIA_IsKeyboardFocusablePropertyId:
			accessibility_detail::SetVariantBool(value, snapshot.Focusable);
			return S_OK;
		case UIA_HasKeyboardFocusPropertyId:
			accessibility_detail::SetVariantBool(value, snapshot.Focused);
			return S_OK;
		case UIA_IsOffscreenPropertyId:
			accessibility_detail::SetVariantBool(value,
				!snapshot.Visible || !IsOnscreen(control));
			return S_OK;
		case UIA_IsPasswordPropertyId:
			accessibility_detail::SetVariantBool(value, snapshot.Password);
			return S_OK;
		case UIA_IsControlElementPropertyId:
		case UIA_IsContentElementPropertyId:
			accessibility_detail::SetVariantBool(value, true);
			return S_OK;
		case UIA_IsInvokePatternAvailablePropertyId:
			accessibility_detail::SetVariantBool(value, SupportsInvoke()); return S_OK;
		case UIA_IsTogglePatternAvailablePropertyId:
			accessibility_detail::SetVariantBool(value, SupportsToggle()); return S_OK;
		case UIA_IsValuePatternAvailablePropertyId:
			accessibility_detail::SetVariantBool(value, SupportsValue()); return S_OK;
		case UIA_IsRangeValuePatternAvailablePropertyId:
			accessibility_detail::SetVariantBool(value, SupportsRange()); return S_OK;
		case UIA_IsExpandCollapsePatternAvailablePropertyId:
			accessibility_detail::SetVariantBool(value, SupportsExpandCollapse()); return S_OK;
		case UIA_IsSelectionItemPatternAvailablePropertyId:
			accessibility_detail::SetVariantBool(value, SupportsSelectionItem()); return S_OK;
		case UIA_IsSelectionPatternAvailablePropertyId:
			accessibility_detail::SetVariantBool(value, SupportsSelection()); return S_OK;
		case UIA_IsGridPatternAvailablePropertyId:
			accessibility_detail::SetVariantBool(value, SupportsGrid()); return S_OK;
		case UIA_IsTablePatternAvailablePropertyId:
			accessibility_detail::SetVariantBool(value, SupportsTable()); return S_OK;
		case UIA_IsScrollPatternAvailablePropertyId:
			accessibility_detail::SetVariantBool(value, SupportsScroll()); return S_OK;
		case UIA_ScrollHorizontalScrollPercentPropertyId:
			if (!SupportsScroll()) return S_OK;
			value->vt = VT_R8;
			return get_HorizontalScrollPercent(&value->dblVal);
		case UIA_ScrollHorizontalViewSizePropertyId:
			if (!SupportsScroll()) return S_OK;
			value->vt = VT_R8;
			return get_HorizontalViewSize(&value->dblVal);
		case UIA_ScrollVerticalScrollPercentPropertyId:
			if (!SupportsScroll()) return S_OK;
			value->vt = VT_R8;
			return get_VerticalScrollPercent(&value->dblVal);
		case UIA_ScrollVerticalViewSizePropertyId:
			if (!SupportsScroll()) return S_OK;
			value->vt = VT_R8;
			return get_VerticalViewSize(&value->dblVal);
		case UIA_ScrollHorizontallyScrollablePropertyId:
			if (!SupportsScroll()) return S_OK;
			{
				BOOL scrollable = FALSE;
				const HRESULT result = get_HorizontallyScrollable(&scrollable);
				if (SUCCEEDED(result)) accessibility_detail::SetVariantBool(
					value, scrollable != FALSE);
				return result;
			}
		case UIA_ScrollVerticallyScrollablePropertyId:
			if (!SupportsScroll()) return S_OK;
			{
				BOOL scrollable = FALSE;
				const HRESULT result = get_VerticallyScrollable(&scrollable);
				if (SUCCEEDED(result)) accessibility_detail::SetVariantBool(
					value, scrollable != FALSE);
				return result;
			}
		case UIA_ValueValuePropertyId:
			if (!SupportsValue()) return S_OK;
			return accessibility_detail::SetVariantString(value,
				snapshot.Password ? std::wstring{} : snapshot.Value);
		case UIA_ValueIsReadOnlyPropertyId:
			if (!SupportsValue()) return S_OK;
			accessibility_detail::SetVariantBool(value, snapshot.ReadOnly); return S_OK;
		case UIA_RangeValueValuePropertyId:
			if (!SupportsRange()) return S_OK;
			value->vt = VT_R8;
			return get_Value(&value->dblVal);
		case UIA_RangeValueMinimumPropertyId:
			if (!SupportsRange()) return S_OK;
			value->vt = VT_R8;
			return RangeMinimum(&value->dblVal);
		case UIA_RangeValueMaximumPropertyId:
			if (!SupportsRange()) return S_OK;
			value->vt = VT_R8;
			return RangeMaximum(&value->dblVal);
		case UIA_RangeValueSmallChangePropertyId:
			if (!SupportsRange()) return S_OK;
			value->vt = VT_R8;
			return get_SmallChange(&value->dblVal);
		case UIA_RangeValueLargeChangePropertyId:
			if (!SupportsRange()) return S_OK;
			value->vt = VT_R8;
			return get_LargeChange(&value->dblVal);
		case UIA_RangeValueIsReadOnlyPropertyId:
			if (!SupportsRange()) return S_OK;
			{
				BOOL readOnly = FALSE;
				const HRESULT result = RangeIsReadOnly(&readOnly);
				if (FAILED(result)) return result;
				accessibility_detail::SetVariantBool(value, readOnly != FALSE);
			}
			return S_OK;
		case UIA_ToggleToggleStatePropertyId:
			if (!SupportsToggle()) return S_OK;
			{
				AutomationToggleState state = AutomationToggleState::Off;
				const HRESULT result = PerformPeerQuery(
					[&state](AutomationPeer& peer) -> HRESULT
					{
						return peer.TryGetToggleState(state)
							? S_OK : UIA_E_NOTSUPPORTED;
					});
				if (FAILED(result)) return result;
				const auto nativeState =
					state == AutomationToggleState::Indeterminate
						? ToggleState_Indeterminate
						: state == AutomationToggleState::On
							? ToggleState_On
							: ToggleState_Off;
				accessibility_detail::SetVariantInt(value, nativeState);
			}
			return S_OK;
		case UIA_ExpandCollapseExpandCollapseStatePropertyId:
			if (!SupportsExpandCollapse()) return S_OK;
			{
				ExpandCollapseState state = ExpandCollapseState_LeafNode;
				const HRESULT result = get_ExpandCollapseState(&state);
				if (FAILED(result)) return result;
				accessibility_detail::SetVariantInt(value, state);
			}
			return S_OK;
		case UIA_SelectionItemIsSelectedPropertyId:
			if (!SupportsSelectionItem()) return S_OK;
			{
				BOOL selected = FALSE;
				const HRESULT result = get_IsSelected(&selected);
				if (FAILED(result)) return result;
				accessibility_detail::SetVariantBool(value, selected != FALSE);
			}
			return S_OK;
		case UIA_SelectionItemSelectionContainerPropertyId:
			if (!SupportsSelectionItem()) return S_OK;
			{
				IRawElementProviderSimple* provider = nullptr;
				const HRESULT result = get_SelectionContainer(&provider);
				if (FAILED(result))
				{
					if (provider) provider->Release();
					return result;
				}
				return accessibility_detail::TakeVariantProvider(value, provider);
			}
		case UIA_SelectionSelectionPropertyId:
			if (!SupportsSelection()) return S_OK;
			{
				SAFEARRAY* providers = nullptr;
				const HRESULT result = GetSelection(&providers);
				if (FAILED(result))
				{
					if (providers) (void)::SafeArrayDestroy(providers);
					return result;
				}
				return accessibility_detail::TakeVariantProviderArray(
					value, providers);
			}
		case UIA_SelectionCanSelectMultiplePropertyId:
			if (!SupportsSelection()) return S_OK;
			{
				BOOL canSelectMultiple = FALSE;
				const HRESULT result = get_CanSelectMultiple(&canSelectMultiple);
				if (FAILED(result)) return result;
				accessibility_detail::SetVariantBool(
					value, canSelectMultiple != FALSE);
				return S_OK;
			}
		case UIA_SelectionIsSelectionRequiredPropertyId:
			if (!SupportsSelection()) return S_OK;
			{
				BOOL selectionRequired = FALSE;
				const HRESULT result = get_IsSelectionRequired(&selectionRequired);
				if (FAILED(result)) return result;
				accessibility_detail::SetVariantBool(
					value, selectionRequired != FALSE);
				return S_OK;
			}
		case UIA_GridRowCountPropertyId:
			if (!SupportsGrid()) return S_OK;
			{
				int rowCount = 0;
				const HRESULT result = get_RowCount(&rowCount);
				if (FAILED(result)) return result;
				accessibility_detail::SetVariantInt(value, rowCount);
				return S_OK;
			}
		case UIA_GridColumnCountPropertyId:
			if (!SupportsGrid()) return S_OK;
			{
				int columnCount = 0;
				const HRESULT result = get_ColumnCount(&columnCount);
				if (FAILED(result)) return result;
				accessibility_detail::SetVariantInt(value, columnCount);
				return S_OK;
			}
		case UIA_TableRowHeadersPropertyId:
		case UIA_TableColumnHeadersPropertyId:
			if (!SupportsTable()) return S_OK;
			{
				SAFEARRAY* providers = nullptr;
				const HRESULT result = propertyId == UIA_TableRowHeadersPropertyId
					? GetRowHeaders(&providers)
					: GetColumnHeaders(&providers);
				if (FAILED(result))
				{
					if (providers) (void)::SafeArrayDestroy(providers);
					return result;
				}
				return accessibility_detail::TakeVariantProviderArray(
					value, providers);
			}
		case UIA_TableRowOrColumnMajorPropertyId:
			if (!SupportsTable()) return S_OK;
			{
				RowOrColumnMajor major = RowOrColumnMajor_Indeterminate;
				const HRESULT result = get_RowOrColumnMajor(&major);
				if (FAILED(result)) return result;
				accessibility_detail::SetVariantInt(
					value, static_cast<int>(major));
				return S_OK;
			}
		default:
			return S_OK;
		}
	}
	catch (const std::bad_alloc&)
	{
		if (value)
		{
			(void)::VariantClear(value);
			::VariantInit(value);
		}
		return E_OUTOFMEMORY;
	}
	catch (...)
	{
		if (value)
		{
			(void)::VariantClear(value);
			::VariantInit(value);
		}
		return UIA_E_INVALIDOPERATION;
	}
	HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
		IRawElementProviderSimple** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		return CurrentControl() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}

	HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
		IRawElementProviderFragment** value) override;
	HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		if (!CurrentControl()) return UIA_E_ELEMENTNOTAVAILABLE;
		SAFEARRAY* result = ::SafeArrayCreateVector(VT_I4, 0, 2);
		if (!result) return E_OUTOFMEMORY;
		LONG first = 0;
		LONG second = 1;
		int append = UiaAppendRuntimeId;
		int id = static_cast<int>(_runtimeId);
		HRESULT hr = ::SafeArrayPutElement(result, &first, &append);
		if (SUCCEEDED(hr)) hr = ::SafeArrayPutElement(result, &second, &id);
		if (FAILED(hr))
		{
			::SafeArrayDestroy(result);
			return hr;
		}
		*value = result;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* value) override try
	{
		if (!value) return E_POINTER;
		*value = UiaRect{};
		auto* control = CurrentControl();
		auto* form = _root ? _root->GetWindow() : nullptr;
		if (!control || !form) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference controlLifetime(control);
		RECT rectangle = form->ContentDipRectToClientPixels(
			control->GetRenderedAbsoluteRectDip());
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		POINT points[2]{ { rectangle.left, rectangle.top },
			{ rectangle.right, rectangle.bottom } };
		::MapWindowPoints(form->Handle, nullptr, points, 2);
		value->left = points[0].x;
		value->top = points[0].y;
		value->width = points[1].x - points[0].x;
		value->height = points[1].y - points[0].y;
		return S_OK;
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		return CurrentControl() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}
	HRESULT STDMETHODCALLTYPE SetFocus() override try
	{
		auto* control = CurrentControl();
		if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference controlLifetime(control);
		const bool enabled = control->GetAccessibilitySnapshot().Enabled;
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!enabled)
			return UIA_E_ELEMENTNOTENABLED;
		const bool focused = control->Focus();
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		return focused ? S_OK : UIA_E_INVALIDOPERATION;
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE get_FragmentRoot(
		IRawElementProviderFragmentRoot** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		if (!CurrentControl() || !_root) return UIA_E_ELEMENTNOTAVAILABLE;
		*value = static_cast<IRawElementProviderFragmentRoot*>(_root);
		_root->AddRef();
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Invoke() override
	try
	{
		return PerformPeerOperation(
			[](AutomationPeer& peer) { return peer.Invoke(); });
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE Toggle() override
	try
	{
		return PerformPeerOperation(
			[](AutomationPeer& peer) { return peer.Toggle(); });
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* value) override
	{
		if (!value) return E_POINTER;
		return PerformPeerQuery([value](AutomationPeer& peer) -> HRESULT
		{
			AutomationToggleState state = AutomationToggleState::Off;
			if (!peer.TryGetToggleState(state)) return UIA_E_NOTSUPPORTED;
			*value = state == AutomationToggleState::Indeterminate
				? ToggleState_Indeterminate
				: state == AutomationToggleState::On
					? ToggleState_On
					: ToggleState_Off;
			return S_OK;
		});
	}

	HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override
	try
	{
		const std::wstring requested(value ? value : L"");
		return PerformPeerOperation(
			[&requested](AutomationPeer& peer)
			{
				return peer.SetValue(requested);
			});
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE get_Value(BSTR* value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		bool supportsValue = false;
		HRESULT result = PerformPeerQuery(
			[&supportsValue](AutomationPeer& peer) -> HRESULT
		{
			supportsValue = peer.SupportsPattern(AutomationPattern::Value);
			return S_OK;
		});
		if (FAILED(result)) return result;
		if (!supportsValue) return UIA_E_NOTSUPPORTED;
		bool password = false;
		result = PerformPeerQuery(
			[&password](AutomationPeer& peer) -> HRESULT
		{
			password = peer.IsPassword();
			return S_OK;
		});
		if (FAILED(result)) return result;
		std::wstring exposed;
		if (!password)
		{
			result = PerformPeerQuery(
				[&exposed](AutomationPeer& peer) -> HRESULT
			{
				exposed = peer.GetValue();
				return S_OK;
			});
			if (FAILED(result)) return result;
		}
		*value = ::SysAllocStringLen(
			exposed.data(), static_cast<UINT>(exposed.size()));
		return *value ? S_OK : E_OUTOFMEMORY;
	}
	HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* value) override
	{
		if (!value) return E_POINTER;
		bool supportsValue = false;
		HRESULT result = PerformPeerQuery(
			[&supportsValue](AutomationPeer& peer) -> HRESULT
		{
			supportsValue = peer.SupportsPattern(AutomationPattern::Value);
			return S_OK;
		});
		if (FAILED(result)) return result;
		if (supportsValue)
		{
			return PerformPeerQuery([value](AutomationPeer& peer) -> HRESULT
			{
				*value = peer.IsReadOnly() ? TRUE : FALSE;
				return S_OK;
			});
		}
		return PerformPeerQuery([value](AutomationPeer& peer) -> HRESULT
		{
			AutomationRangeValue range;
			if (!peer.TryGetRangeValue(range)) return UIA_E_NOTSUPPORTED;
			*value = range.IsReadOnly ? TRUE : FALSE;
			return S_OK;
		});
	}

	HRESULT STDMETHODCALLTYPE SetValue(double value) override
	try
	{
		return PerformPeerOperation(
			[value](AutomationPeer& peer)
			{
				return peer.SetRangeValue(value);
			});
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE get_Value(double* value) override
	{
		if (!value) return E_POINTER;
		return PerformRangeQuery(value,
			[](const AutomationRangeValue& range) { return range.Value; });
	}
	HRESULT STDMETHODCALLTYPE get_Maximum(double* value) override { return RangeMaximum(value); }
	HRESULT STDMETHODCALLTYPE get_Minimum(double* value) override { return RangeMinimum(value); }
	HRESULT STDMETHODCALLTYPE get_LargeChange(double* value) override
	{
		if (!value) return E_POINTER;
		return PerformRangeQuery(value,
			[](const AutomationRangeValue& range) { return range.LargeChange; });
	}
	HRESULT STDMETHODCALLTYPE get_SmallChange(double* value) override
	{
		if (!value) return E_POINTER;
		return PerformRangeQuery(value,
			[](const AutomationRangeValue& range) { return range.SmallChange; });
	}

	HRESULT STDMETHODCALLTYPE Expand() override try
	{
		return SetExpandedState(true);
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE Collapse() override try
	{
		return SetExpandedState(false);
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(
		ExpandCollapseState* value) override
	{
		if (!value) return E_POINTER;
		return PerformPeerQuery([value](AutomationPeer& peer) -> HRESULT
		{
			bool expanded = false;
			if (!peer.TryGetExpanded(expanded)) return UIA_E_NOTSUPPORTED;
			*value = expanded ? ExpandCollapseState_Expanded
				: ExpandCollapseState_Collapsed;
			return S_OK;
		});
	}

	HRESULT STDMETHODCALLTYPE Select() override try
	{
		return SelectItem();
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE AddToSelection() override try
	{
		return SelectItem();
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE RemoveFromSelection() override
	{
		return CurrentControl() ? UIA_E_INVALIDOPERATION
			: UIA_E_ELEMENTNOTAVAILABLE;
	}
	HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* value) override
	{
		if (!value) return E_POINTER;
		*value = FALSE;
		return PerformPeerQuery([value](AutomationPeer& peer) -> HRESULT
		{
			bool selected = false;
			if (!peer.TryGetSelectionItemSelected(selected))
				return UIA_E_NOTSUPPORTED;
			*value = selected ? TRUE : FALSE;
			return S_OK;
		});
	}
	HRESULT STDMETHODCALLTYPE get_SelectionContainer(
		IRawElementProviderSimple** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		try
		{
			auto* control = CurrentControl();
			if (!control || !_root) return UIA_E_ELEMENTNOTAVAILABLE;
			const ControlWeakReference controlLifetime(control);
			auto peer = control->AcquireAutomationPeer();
			if (!peer) return UIA_E_ELEMENTNOTAVAILABLE;
			auto* container = peer->GetSelectionContainer();

			control = controlLifetime.Get();
			if (!control || CurrentControl() != control)
				return UIA_E_ELEMENTNOTAVAILABLE;
			if (container)
			{
				container = _root->ResolveControl(container);
				if (!container) return UIA_E_ELEMENTNOTAVAILABLE;
				const uint32_t containerRuntimeId =
					container->GetAccessibilityRuntimeId();
				if (_root->ResolveControl(container, containerRuntimeId)
					!= container) return UIA_E_ELEMENTNOTAVAILABLE;
				auto* provider = _root->ProviderFor(container);
				if (!provider) return UIA_E_ELEMENTNOTAVAILABLE;
				*value = static_cast<IRawElementProviderSimple*>(provider);
			}
			else if (auto* parent = control->GetVisualParent())
			{
				const uint32_t parentRuntimeId =
					parent->GetAccessibilityRuntimeId();
				if (_root->ResolveControl(parent, parentRuntimeId) != parent)
					return UIA_E_ELEMENTNOTAVAILABLE;
				auto* provider = _root->ProviderFor(parent);
				if (!provider) return UIA_E_ELEMENTNOTAVAILABLE;
				*value = static_cast<IRawElementProviderSimple*>(provider);
			}
			else
			{
				*value = static_cast<IRawElementProviderSimple*>(_root);
				_root->AddRef();
			}
			return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}

	HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** value) override;
	HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* value) override
	{
		if (!value) return E_POINTER;
		*value = FALSE;
		try
		{
			auto* control = CurrentControl();
			if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
			const ControlWeakReference controlLifetime(control);
			if (auto source = VirtualSourceLease())
			{
				const auto info = source->GetAccessibilityVirtualContainerInfo();
				control = controlLifetime.Get();
				if (!control || CurrentControl() != control)
					return UIA_E_ELEMENTNOTAVAILABLE;
				if (HasAutomationPattern(
					info.Patterns, AutomationPattern::Selection))
				{
					*value = info.CanSelectMultiple ? TRUE : FALSE;
					return S_OK;
				}
			}
			auto peer = CurrentPeerLease();
			if (!peer) return UIA_E_ELEMENTNOTAVAILABLE;
			const bool supportsSelection =
				peer->SupportsPattern(AutomationPattern::Selection);
			control = controlLifetime.Get();
			if (!control || CurrentControl() != control)
				return UIA_E_ELEMENTNOTAVAILABLE;
			if (!supportsSelection)
				return UIA_E_NOTSUPPORTED;
			const bool canSelectMultiple = peer->CanSelectMultiple();
			control = controlLifetime.Get();
			if (!control || CurrentControl() != control)
				return UIA_E_ELEMENTNOTAVAILABLE;
			*value = canSelectMultiple ? TRUE : FALSE;
			return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* value) override
	{
		if (!value) return E_POINTER;
		*value = FALSE;
		try
		{
			auto* control = CurrentControl();
			if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
			const ControlWeakReference controlLifetime(control);
			if (auto source = VirtualSourceLease())
			{
				const auto info = source->GetAccessibilityVirtualContainerInfo();
				control = controlLifetime.Get();
				if (!control || CurrentControl() != control)
					return UIA_E_ELEMENTNOTAVAILABLE;
				if (HasAutomationPattern(
					info.Patterns, AutomationPattern::Selection))
				{
					*value = info.IsSelectionRequired ? TRUE : FALSE;
					return S_OK;
				}
			}
			auto peer = CurrentPeerLease();
			if (!peer) return UIA_E_ELEMENTNOTAVAILABLE;
			const bool supportsSelection =
				peer->SupportsPattern(AutomationPattern::Selection);
			control = controlLifetime.Get();
			if (!control || CurrentControl() != control)
				return UIA_E_ELEMENTNOTAVAILABLE;
			if (!supportsSelection)
				return UIA_E_NOTSUPPORTED;
			const bool selectionRequired = peer->IsSelectionRequired();
			control = controlLifetime.Get();
			if (!control || CurrentControl() != control)
				return UIA_E_ELEMENTNOTAVAILABLE;
			*value = selectionRequired ? TRUE : FALSE;
			return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	HRESULT STDMETHODCALLTYPE GetItem(
		int row, int column, IRawElementProviderSimple** value) override;
	HRESULT STDMETHODCALLTYPE get_RowCount(int* value) override;
	HRESULT STDMETHODCALLTYPE get_ColumnCount(int* value) override;
	HRESULT STDMETHODCALLTYPE GetRowHeaders(SAFEARRAY** value) override;
	HRESULT STDMETHODCALLTYPE GetColumnHeaders(SAFEARRAY** value) override;
	HRESULT STDMETHODCALLTYPE get_RowOrColumnMajor(
		RowOrColumnMajor* value) override;
	HRESULT STDMETHODCALLTYPE Scroll(
		ScrollAmount horizontalAmount, ScrollAmount verticalAmount) override;
	HRESULT STDMETHODCALLTYPE SetScrollPercent(
		double horizontalPercent, double verticalPercent) override;
	HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double* value) override;
	HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double* value) override;
	HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double* value) override;
	HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double* value) override;
	HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(BOOL* value) override;
	HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(BOOL* value) override;

private:
	~ControlUiaProvider()
	{
		if (_root)
		{
			_root->UnregisterProvider(_runtimeId, this);
			_root->Release();
		}
	}
	Control* CurrentControl() const
	{
		return _root ? _root->ResolveControl(_control, _runtimeId) : nullptr;
	}
	std::shared_ptr<AutomationPeer> CurrentPeerLease() const
	{
		auto* control = CurrentControl();
		if (!control) return {};
		const ControlWeakReference controlLifetime(control);
		auto peer = control->AcquireAutomationPeer();
		control = controlLifetime.Get();
		return control && CurrentControl() == control ? peer : nullptr;
	}
	template<class Operation>
	HRESULT PerformPeerOperation(Operation&& operation)
	{
		auto* control = CurrentControl();
		if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference controlLifetime(control);
		auto peer = CurrentPeerLease();
		if (!peer) return UIA_E_ELEMENTNOTAVAILABLE;
		const auto result = operation(*peer);
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		return accessibility_detail::ToHresult(result);
	}
	template<class Query>
	HRESULT PerformPeerQuery(Query&& query) noexcept
	{
		try
		{
			auto* control = CurrentControl();
			if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
			const ControlWeakReference controlLifetime(control);
			auto peer = control->AcquireAutomationPeer();
			if (!peer) return UIA_E_ELEMENTNOTAVAILABLE;
			const HRESULT result = query(*peer);
			control = controlLifetime.Get();
			if (!control || CurrentControl() != control)
				return UIA_E_ELEMENTNOTAVAILABLE;
			return result;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	template<class Selector>
	HRESULT PerformRangeQuery(double* value, Selector&& selector) noexcept
	{
		return PerformPeerQuery(
			[value, &selector](AutomationPeer& peer) -> HRESULT
		{
			AutomationRangeValue range;
			if (!peer.TryGetRangeValue(range)) return UIA_E_NOTSUPPORTED;
			*value = selector(range);
			return S_OK;
		});
	}
	bool SupportsPeerPattern(AutomationPattern pattern) const
	{
		auto* control = CurrentControl();
		if (!control) return false;
		const ControlWeakReference controlLifetime(control);
		auto peer = control->AcquireAutomationPeer();
		if (!peer) return false;
		const bool supported = peer->SupportsPattern(pattern);
		control = controlLifetime.Get();
		return control && CurrentControl() == control && supported;
	}
	bool SupportsInvoke() const
	{
		return SupportsPeerPattern(AutomationPattern::Invoke);
	}
	bool SupportsToggle() const
	{
		return SupportsPeerPattern(AutomationPattern::Toggle);
	}
	bool SupportsValue() const
	{
		return SupportsPeerPattern(AutomationPattern::Value);
	}
	bool SupportsRange() const
	{
		return SupportsPeerPattern(AutomationPattern::RangeValue);
	}
	bool SupportsExpandCollapse() const
	{
		return SupportsPeerPattern(AutomationPattern::ExpandCollapse);
	}
	bool SupportsSelectionItem() const
	{
		return SupportsPeerPattern(AutomationPattern::SelectionItem);
	}
	AccessibilityVirtualContainerInfo VirtualContainerInfo() const
	{
		AccessibilityVirtualContainerInfo result;
		(void)TryGetVirtualContainerInfo(result);
		return result;
	}
	std::shared_ptr<AutomationPeer> VirtualSourceLease() const
	{
		auto* control = CurrentControl();
		if (!control) return {};
		const ControlWeakReference controlLifetime(control);
		auto peer = control->AcquireAutomationPeer();
		if (!peer || !peer->SupportsVirtualizedChildren()) return {};
		control = controlLifetime.Get();
		return control && CurrentControl() == control ? peer : nullptr;
	}
	HRESULT TryGetVirtualContainerInfo(
		AccessibilityVirtualContainerInfo& result) const
	{
		auto* control = CurrentControl();
		if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference controlLifetime(control);
		auto source = VirtualSourceLease();
		if (!source) return UIA_E_NOTSUPPORTED;
		result = source->GetAccessibilityVirtualContainerInfo();
		control = controlLifetime.Get();
		return control && CurrentControl() == control
			? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}
	HRESULT TryGetScrollInfo(AccessibilityScrollInfo& result) const
	{
		auto* control = CurrentControl();
		if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference controlLifetime(control);
		auto source = VirtualSourceLease();
		if (!source) return UIA_E_NOTSUPPORTED;
		const auto container = source->GetAccessibilityVirtualContainerInfo();
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(container.Patterns, AutomationPattern::Scroll)
			|| !source->GetAccessibilityScrollInfo(result))
			return UIA_E_NOTSUPPORTED;
		control = controlLifetime.Get();
		return control && CurrentControl() == control
			? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}
	bool SupportsSelection() const
	{
		return SupportsPeerPattern(AutomationPattern::Selection)
			|| HasAutomationPattern(
				VirtualContainerInfo().Patterns,
				AutomationPattern::Selection);
	}
	bool SupportsGrid() const
	{
		return HasAutomationPattern(
			VirtualContainerInfo().Patterns,
			AutomationPattern::Grid);
	}
	bool SupportsTable() const
	{
		return HasAutomationPattern(
			VirtualContainerInfo().Patterns,
			AutomationPattern::Table);
	}
	bool SupportsScroll() const
	{
		return HasAutomationPattern(
			VirtualContainerInfo().Patterns,
			AutomationPattern::Scroll);
	}
	bool IsOnscreen(Control* control) const
	{
		auto* form = _root ? _root->GetWindow() : nullptr;
		if (!form || !form->Handle) return false;
		RECT controlRect = form->ContentDipRectToClientPixels(
			control->GetRenderedAbsoluteRectDip());
		RECT client{};
		return ::GetClientRect(form->Handle, &client)
			&& controlRect.right > client.left && controlRect.left < client.right
			&& controlRect.bottom > client.top && controlRect.top < client.bottom;
	}
	HRESULT RangeIsReadOnly(BOOL* value)
	{
		if (!value) return E_POINTER;
		return PerformPeerQuery([value](AutomationPeer& peer) -> HRESULT
		{
			AutomationRangeValue range;
			if (!peer.TryGetRangeValue(range)) return UIA_E_NOTSUPPORTED;
			*value = range.IsReadOnly ? TRUE : FALSE;
			return S_OK;
		});
	}
	HRESULT RangeMaximum(double* value)
	{
		if (!value) return E_POINTER;
		return PerformRangeQuery(value,
			[](const AutomationRangeValue& range) { return range.Maximum; });
	}
	HRESULT RangeMinimum(double* value)
	{
		if (!value) return E_POINTER;
		return PerformRangeQuery(value,
			[](const AutomationRangeValue& range) { return range.Minimum; });
	}
	HRESULT SetExpandedState(bool expanded)
	{
		return PerformPeerOperation(
			[expanded](AutomationPeer& peer)
			{
				return peer.SetExpanded(expanded);
			});
	}
	HRESULT SelectItem()
	{
		return PerformPeerOperation(
			[](AutomationPeer& peer) { return peer.Select(); });
	}
	HRESULT CreateVirtualProviderArray(
		const std::vector<uint32_t>& ids, SAFEARRAY** value);

	std::atomic<ULONG> _references{ 1 };
	WindowUiaProvider* _root = nullptr;
	Control* _control = nullptr;
	uint32_t _runtimeId = 0;
};

class VirtualUiaProvider final :
	public IRawElementProviderSimple,
	public IRawElementProviderFragment,
	public IInvokeProvider,
	public IToggleProvider,
	public IValueProvider,
	public IExpandCollapseProvider,
	public ISelectionItemProvider,
	public ISelectionProvider,
	public IScrollItemProvider,
	public IVirtualizedItemProvider,
	public IGridItemProvider,
	public ITableItemProvider
{
public:
	VirtualUiaProvider(WindowUiaProvider* root, Control* owner, uint32_t id) :
		_root(root), _owner(owner),
		_ownerRuntimeId(owner ? owner->GetAccessibilityRuntimeId() : 0),
		_virtualId(id),
		_key((static_cast<uint64_t>(_ownerRuntimeId) << 32) | id)
	{
		if (_root) _root->AddRef();
	}

	bool Matches(Control* owner, uint32_t id) const noexcept
	{
		return owner == _owner && id == _virtualId
			&& owner && owner->GetAccessibilityRuntimeId() == _ownerRuntimeId;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
	{
		if (!object) return E_POINTER;
		*object = nullptr;
		if (iid == IID_IUnknown || iid == IID_IRawElementProviderSimple)
			*object = static_cast<IRawElementProviderSimple*>(this);
		else if (iid == IID_IRawElementProviderFragment)
			*object = static_cast<IRawElementProviderFragment*>(this);
		else if (iid == IID_IInvokeProvider && Supports(
			AutomationPattern::Invoke))
			*object = static_cast<IInvokeProvider*>(this);
		else if (iid == IID_IToggleProvider && Supports(
			AutomationPattern::Toggle))
			*object = static_cast<IToggleProvider*>(this);
		else if (iid == IID_IValueProvider && Supports(
			AutomationPattern::Value))
			*object = static_cast<IValueProvider*>(this);
		else if (iid == IID_IExpandCollapseProvider && Supports(
			AutomationPattern::ExpandCollapse))
			*object = static_cast<IExpandCollapseProvider*>(this);
		else if (iid == IID_ISelectionItemProvider && Supports(
			AutomationPattern::SelectionItem))
			*object = static_cast<ISelectionItemProvider*>(this);
		else if (iid == IID_ISelectionProvider && Supports(
			AutomationPattern::Selection))
			*object = static_cast<ISelectionProvider*>(this);
		else if (iid == IID_IScrollItemProvider && Supports(
			AutomationPattern::ScrollItem))
			*object = static_cast<IScrollItemProvider*>(this);
		else if (iid == IID_IVirtualizedItemProvider && Supports(
			AutomationPattern::VirtualizedItem))
			*object = static_cast<IVirtualizedItemProvider*>(this);
		else if (iid == IID_IGridItemProvider && Supports(
			AutomationPattern::GridItem))
			*object = static_cast<IGridItemProvider*>(this);
		else if (iid == IID_ITableItemProvider && Supports(
			AutomationPattern::TableItem))
			*object = static_cast<ITableItemProvider*>(this);
		else
			return E_NOINTERFACE;
		AddRef();
		return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++_references; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG remaining = --_references;
		if (remaining == 0) delete this;
		return remaining;
	}

	HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* value) override
	{
		if (!value) return E_POINTER;
		*value = ProviderOptions_ServerSideProvider;
		return CurrentNode(nullptr) ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}
	HRESULT STDMETHODCALLTYPE GetPatternProvider(
		PATTERNID patternId, IUnknown** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		if (!CurrentNode(nullptr)) return UIA_E_ELEMENTNOTAVAILABLE;
		const IID* iid = nullptr;
		if (patternId == UIA_InvokePatternId) iid = &IID_IInvokeProvider;
		else if (patternId == UIA_TogglePatternId) iid = &IID_IToggleProvider;
		else if (patternId == UIA_ValuePatternId) iid = &IID_IValueProvider;
		else if (patternId == UIA_ExpandCollapsePatternId) iid = &IID_IExpandCollapseProvider;
		else if (patternId == UIA_SelectionItemPatternId) iid = &IID_ISelectionItemProvider;
		else if (patternId == UIA_SelectionPatternId) iid = &IID_ISelectionProvider;
		else if (patternId == UIA_ScrollItemPatternId) iid = &IID_IScrollItemProvider;
		else if (patternId == UIA_VirtualizedItemPatternId) iid = &IID_IVirtualizedItemProvider;
		else if (patternId == UIA_GridItemPatternId) iid = &IID_IGridItemProvider;
		else if (patternId == UIA_TableItemPatternId) iid = &IID_ITableItemProvider;
		else return S_OK;
		const HRESULT result = QueryInterface(*iid, reinterpret_cast<void**>(value));
		return result == E_NOINTERFACE ? S_OK : result;
	}
	HRESULT STDMETHODCALLTYPE GetPropertyValue(
		PROPERTYID propertyId, VARIANT* value) override
	{
		if (!value) return E_POINTER;
		::VariantInit(value);
		try
		{
		AccessibilityVirtualNode node;
		if (!CurrentNode(&node)) return UIA_E_ELEMENTNOTAVAILABLE;
		switch (propertyId)
		{
		case UIA_NamePropertyId:
			return accessibility_detail::SetVariantString(value, node.Name);
		case UIA_AutomationIdPropertyId:
			return accessibility_detail::SetVariantString(value, node.AutomationId);
		case UIA_ControlTypePropertyId:
			accessibility_detail::SetVariantInt(value,
				accessibility_detail::ToUiaControlType(node.ControlType)); return S_OK;
		case UIA_ClassNamePropertyId:
			return accessibility_detail::SetVariantString(value,
				node.ClassName.empty() ? L"CUI.VirtualItem" : node.ClassName);
		case UIA_FrameworkIdPropertyId:
			return accessibility_detail::SetVariantString(value, L"CUI");
		case UIA_ProviderDescriptionPropertyId:
			return accessibility_detail::SetVariantString(
				value, L"CUI native virtual item provider");
		case UIA_HelpTextPropertyId:
		case UIA_FullDescriptionPropertyId:
			return accessibility_detail::SetVariantString(value, node.Description);
		case UIA_IsEnabledPropertyId:
			accessibility_detail::SetVariantBool(value, node.Enabled); return S_OK;
		case UIA_IsKeyboardFocusablePropertyId:
			accessibility_detail::SetVariantBool(
				value, node.KeyboardFocusable);
			return S_OK;
		case UIA_HasKeyboardFocusPropertyId:
			{
				if (!_root || !_root->HasNativeKeyboardFocus())
				{
					accessibility_detail::SetVariantBool(value, false);
					return S_OK;
				}
				if (node.HasKeyboardFocus)
				{
					accessibility_detail::SetVariantBool(value, true);
					return S_OK;
				}
				auto* owner = _root
					? _root->ResolveControl(_owner, _ownerRuntimeId) : nullptr;
				if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
				const ControlWeakReference ownerLifetime(owner);
				const auto snapshot = owner->GetAccessibilitySnapshot();
				owner = ownerLifetime.Get();
				if (!owner || !_root
					|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner)
					return UIA_E_ELEMENTNOTAVAILABLE;
				accessibility_detail::SetVariantBool(value,
					snapshot.Focused
					&& _root->IsVirtualFocused(owner, _virtualId));
				return S_OK;
			}
		case UIA_IsOffscreenPropertyId:
			accessibility_detail::SetVariantBool(value, !node.Visible); return S_OK;
		case UIA_IsControlElementPropertyId:
			accessibility_detail::SetVariantBool(
				value, node.IsControlElement); return S_OK;
		case UIA_IsContentElementPropertyId:
			accessibility_detail::SetVariantBool(
				value, node.IsContentElement); return S_OK;
		case UIA_IsInvokePatternAvailablePropertyId:
			return SetPatternAvailability(value, node, AutomationPattern::Invoke);
		case UIA_IsTogglePatternAvailablePropertyId:
			return SetPatternAvailability(value, node, AutomationPattern::Toggle);
		case UIA_IsValuePatternAvailablePropertyId:
			return SetPatternAvailability(value, node, AutomationPattern::Value);
		case UIA_IsExpandCollapsePatternAvailablePropertyId:
			return SetPatternAvailability(value, node, AutomationPattern::ExpandCollapse);
		case UIA_IsSelectionItemPatternAvailablePropertyId:
			return SetPatternAvailability(value, node, AutomationPattern::SelectionItem);
		case UIA_IsSelectionPatternAvailablePropertyId:
			return SetPatternAvailability(value, node, AutomationPattern::Selection);
		case UIA_IsScrollItemPatternAvailablePropertyId:
			return SetPatternAvailability(value, node, AutomationPattern::ScrollItem);
		case UIA_IsVirtualizedItemPatternAvailablePropertyId:
			return SetPatternAvailability(value, node, AutomationPattern::VirtualizedItem);
		case UIA_IsGridItemPatternAvailablePropertyId:
			return SetPatternAvailability(value, node, AutomationPattern::GridItem);
		case UIA_IsTableItemPatternAvailablePropertyId:
			return SetPatternAvailability(value, node, AutomationPattern::TableItem);
		case UIA_ToggleToggleStatePropertyId:
			if (!Supports(AutomationPattern::Toggle)) return S_OK;
			accessibility_detail::SetVariantInt(
				value, node.Checked ? ToggleState_On : ToggleState_Off); return S_OK;
		case UIA_ValueValuePropertyId:
			if (!Supports(AutomationPattern::Value)) return S_OK;
			return accessibility_detail::SetVariantString(value, node.Value);
		case UIA_ValueIsReadOnlyPropertyId:
			if (!Supports(AutomationPattern::Value)) return S_OK;
			accessibility_detail::SetVariantBool(value, node.ReadOnly); return S_OK;
		case UIA_ExpandCollapseExpandCollapseStatePropertyId:
			if (!Supports(AutomationPattern::ExpandCollapse)) return S_OK;
			accessibility_detail::SetVariantInt(value, node.Expanded
				? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed); return S_OK;
		case UIA_SelectionItemIsSelectedPropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::SelectionItem)) return S_OK;
			accessibility_detail::SetVariantBool(value, node.Selected); return S_OK;
		case UIA_SelectionItemSelectionContainerPropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::SelectionItem)) return S_OK;
			{
				IRawElementProviderSimple* provider = nullptr;
				const HRESULT result = get_SelectionContainer(&provider);
				if (FAILED(result))
				{
					if (provider) provider->Release();
					return result;
				}
				return accessibility_detail::TakeVariantProvider(value, provider);
			}
		case UIA_SelectionSelectionPropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::Selection)) return S_OK;
			{
				SAFEARRAY* providers = nullptr;
				const HRESULT result = GetSelection(&providers);
				if (FAILED(result))
				{
					if (providers) (void)::SafeArrayDestroy(providers);
					return result;
				}
				return accessibility_detail::TakeVariantProviderArray(
					value, providers);
			}
		case UIA_SelectionCanSelectMultiplePropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::Selection)) return S_OK;
			{
				BOOL canSelectMultiple = FALSE;
				const HRESULT result = get_CanSelectMultiple(
					&canSelectMultiple);
				if (FAILED(result)) return result;
				accessibility_detail::SetVariantBool(
					value, canSelectMultiple != FALSE);
				return S_OK;
			}
		case UIA_SelectionIsSelectionRequiredPropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::Selection)) return S_OK;
			{
				BOOL selectionRequired = FALSE;
				const HRESULT result = get_IsSelectionRequired(
					&selectionRequired);
				if (FAILED(result)) return result;
				accessibility_detail::SetVariantBool(
					value, selectionRequired != FALSE);
				return S_OK;
			}
		case UIA_GridItemRowPropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::GridItem)) return S_OK;
			accessibility_detail::SetVariantInt(value, node.Row); return S_OK;
		case UIA_GridItemColumnPropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::GridItem)) return S_OK;
			accessibility_detail::SetVariantInt(value, node.Column); return S_OK;
		case UIA_GridItemRowSpanPropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::GridItem)) return S_OK;
			accessibility_detail::SetVariantInt(value, node.RowSpan); return S_OK;
		case UIA_GridItemColumnSpanPropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::GridItem)) return S_OK;
			accessibility_detail::SetVariantInt(value, node.ColumnSpan); return S_OK;
		case UIA_GridItemContainingGridPropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::GridItem)) return S_OK;
			{
				IRawElementProviderSimple* provider = nullptr;
				const HRESULT result = get_ContainingGrid(&provider);
				if (FAILED(result))
				{
					if (provider) provider->Release();
					return result;
				}
				return accessibility_detail::TakeVariantProvider(value, provider);
			}
		case UIA_TableItemRowHeaderItemsPropertyId:
		case UIA_TableItemColumnHeaderItemsPropertyId:
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::TableItem)) return S_OK;
			{
				SAFEARRAY* providers = nullptr;
				const HRESULT result =
					propertyId == UIA_TableItemRowHeaderItemsPropertyId
						? GetRowHeaderItems(&providers)
						: GetColumnHeaderItems(&providers);
				if (FAILED(result))
				{
					if (providers) (void)::SafeArrayDestroy(providers);
					return result;
				}
				return accessibility_detail::TakeVariantProviderArray(
					value, providers);
			}
		case UIA_LevelPropertyId:
			accessibility_detail::SetVariantInt(value, node.Level); return S_OK;
		default: return S_OK;
		}
		}
		catch (const std::bad_alloc&)
		{
			::VariantClear(value);
			::VariantInit(value);
			return E_OUTOFMEMORY;
		}
		catch (...)
		{
			::VariantClear(value);
			::VariantInit(value);
			return UIA_E_INVALIDOPERATION;
		}
	}
	HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
		IRawElementProviderSimple** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		return CurrentNode(nullptr) ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}

	HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
		IRawElementProviderFragment** value) override;
	HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		if (!CurrentNode(nullptr)) return UIA_E_ELEMENTNOTAVAILABLE;
		SAFEARRAY* result = ::SafeArrayCreateVector(VT_I4, 0, 3);
		if (!result) return E_OUTOFMEMORY;
		int ids[3]{ UiaAppendRuntimeId, static_cast<int>(_ownerRuntimeId),
			static_cast<int>(_virtualId) };
		HRESULT hr = S_OK;
		for (LONG index = 0; index < 3 && SUCCEEDED(hr); ++index)
			hr = ::SafeArrayPutElement(result, &index, &ids[index]);
		if (FAILED(hr)) { ::SafeArrayDestroy(result); return hr; }
		*value = result;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* value) override try
	{
		if (!value) return E_POINTER;
		*value = UiaRect{};
		AccessibilityVirtualNode node;
		auto* form = _root ? _root->GetWindow() : nullptr;
		if (!CurrentNode(&node) || !form || !_root)
			return UIA_E_ELEMENTNOTAVAILABLE;
		auto* owner = _root->ResolveControl(_owner, _ownerRuntimeId);
		if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference ownerLifetime(owner);
		D2D1_RECT_F rendered = node.BoundsDip;
		if (!node.BoundsAreRenderSpace)
		{
			const auto location = owner->GetAbsoluteLocationDip();
			owner = ownerLifetime.Get();
			if (!owner || _root->ResolveControl(
				_owner, _ownerRuntimeId) != owner)
				return UIA_E_ELEMENTNOTAVAILABLE;
			const D2D1_RECT_F absolute = D2D1::RectF(
				location.x + node.BoundsDip.left, location.y + node.BoundsDip.top,
				location.x + node.BoundsDip.right, location.y + node.BoundsDip.bottom);
			rendered = owner->TransformAbsoluteRectToRenderSpace(absolute);
			owner = ownerLifetime.Get();
			if (!owner || _root->ResolveControl(
				_owner, _ownerRuntimeId) != owner)
				return UIA_E_ELEMENTNOTAVAILABLE;
		}
		RECT rectangle = form->ContentDipRectToClientPixels(rendered);
		POINT points[2]{ { rectangle.left, rectangle.top },
			{ rectangle.right, rectangle.bottom } };
		::MapWindowPoints(form->Handle, nullptr, points, 2);
		value->left = points[0].x;
		value->top = points[0].y;
		value->width = points[1].x - points[0].x;
		value->height = points[1].y - points[0].y;
		return S_OK;
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		return CurrentNode(nullptr) ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
	}
	HRESULT STDMETHODCALLTYPE SetFocus() override
	{
		try
		{
			AccessibilityVirtualNode node;
			AutomationPeer* source = nullptr;
			std::shared_ptr<AutomationPeer> sourceLease;
			if (!CurrentNode(&node, &source, &sourceLease) || !_root)
				return UIA_E_ELEMENTNOTAVAILABLE;
			if (!node.Enabled) return UIA_E_ELEMENTNOTENABLED;

			auto* owner = _root->ResolveControl(_owner, _ownerRuntimeId);
			if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
			const ControlWeakReference ownerLifetime(owner);
			auto* form = _root->GetWindow();
			if (!form || !form->Handle || !::IsWindow(form->Handle))
				return UIA_E_ELEMENTNOTAVAILABLE;
			if (!_root->HasNativeKeyboardFocus())
			{
				(void)::SetFocus(form->Handle);
				owner = ownerLifetime.Get();
				if (!owner
					|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner
					|| !CurrentNode(&node, &source, &sourceLease))
					return UIA_E_ELEMENTNOTAVAILABLE;
				if (!_root->HasNativeKeyboardFocus())
					return UIA_E_INVALIDOPERATION;
			}
			const auto focused = source->FocusAccessibilityVirtualNode(_virtualId);
			if (focused != AutomationOperationResult::Succeeded)
				return accessibility_detail::ToHresult(focused);

			owner = ownerLifetime.Get();
			if (!owner
				|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner
				|| !CurrentNode(&node, &source))
				return UIA_E_ELEMENTNOTAVAILABLE;
			if (!node.Enabled) return UIA_E_ELEMENTNOTENABLED;
			if (!node.KeyboardFocusable)
				return UIA_E_INVALIDOPERATION;
			if (!_root->HasNativeKeyboardFocus())
				return UIA_E_INVALIDOPERATION;

			_root->SetVirtualFocus(owner, _virtualId);
			owner = ownerLifetime.Get();
			if (!owner
				|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner)
				return UIA_E_ELEMENTNOTAVAILABLE;
			// A real DataGridCell focus transition is routed to this virtual
			// provider by RaiseEvent. Owner-backed virtual items still need an
			// explicit event after their virtual-focus identity is committed.
			if (!node.HasKeyboardFocus)
				_root->RaiseVirtualEvent(
					owner, _virtualId, AccessibilityChange::Focus);
			return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	HRESULT STDMETHODCALLTYPE get_FragmentRoot(
		IRawElementProviderFragmentRoot** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		if (!CurrentNode(nullptr) || !_root) return UIA_E_ELEMENTNOTAVAILABLE;
		*value = static_cast<IRawElementProviderFragmentRoot*>(_root);
		_root->AddRef();
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Invoke() override
	{
		return PerformNodeAction(AutomationPattern::Invoke,
			[](AutomationPeer& source, uint32_t id)
			{ return source.InvokeAccessibilityVirtualNode(id); });
	}
	HRESULT STDMETHODCALLTYPE Toggle() override
	{
		return PerformNodeAction(AutomationPattern::Toggle,
			[](AutomationPeer& source, uint32_t id)
			{ return source.ToggleAccessibilityVirtualNode(id); });
	}
	HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* value) override
	{
		if (!value) return E_POINTER;
		AccessibilityVirtualNode node;
		if (!CurrentNode(&node)) return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::Toggle)) return UIA_E_NOTSUPPORTED;
		*value = node.Checked ? ToggleState_On : ToggleState_Off;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override
	{
		if (!value) return E_INVALIDARG;
		try
		{
			AccessibilityVirtualNode node;
			AutomationPeer* source = nullptr;
			std::shared_ptr<AutomationPeer> sourceLease;
			if (!CurrentNode(&node, &source, &sourceLease))
				return UIA_E_ELEMENTNOTAVAILABLE;
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::Value))
				return UIA_E_NOTSUPPORTED;
			if (!node.Enabled) return UIA_E_ELEMENTNOTENABLED;
			if (node.ReadOnly) return UIA_E_NOTSUPPORTED;
			return source->SetAccessibilityVirtualNodeValue(_virtualId, value)
				? S_OK : UIA_E_INVALIDOPERATION;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	HRESULT STDMETHODCALLTYPE get_Value(BSTR* value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		AccessibilityVirtualNode node;
		if (!CurrentNode(&node)) return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::Value)) return UIA_E_NOTSUPPORTED;
		return accessibility_detail::CopyBstr(node.Value, value);
	}
	HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* value) override
	{
		if (!value) return E_POINTER;
		AccessibilityVirtualNode node;
		if (!CurrentNode(&node)) return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::Value)) return UIA_E_NOTSUPPORTED;
		*value = node.ReadOnly ? TRUE : FALSE;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Expand() override try { return SetExpanded(true); }
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE Collapse() override try { return SetExpanded(false); }
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
	HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(
		ExpandCollapseState* value) override
	{
		if (!value) return E_POINTER;
		AccessibilityVirtualNode node;
		if (!CurrentNode(&node)) return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::ExpandCollapse))
			return UIA_E_NOTSUPPORTED;
		*value = node.Expanded ? ExpandCollapseState_Expanded
			: ExpandCollapseState_Collapsed;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Select() override
	{
		return SelectNode(AccessibilitySelectionAction::Select);
	}
	HRESULT STDMETHODCALLTYPE AddToSelection() override
	{
		return SelectNode(AccessibilitySelectionAction::Add);
	}
	HRESULT STDMETHODCALLTYPE RemoveFromSelection() override
	{
		return SelectNode(AccessibilitySelectionAction::Remove);
	}
	HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* value) override
	{
		if (!value) return E_POINTER;
		AccessibilityVirtualNode node;
		if (!CurrentNode(&node)) return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::SelectionItem))
			return UIA_E_NOTSUPPORTED;
		*value = node.Selected ? TRUE : FALSE;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE get_SelectionContainer(
		IRawElementProviderSimple** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		try
		{
			if (!CurrentNode(nullptr) || !_root)
				return UIA_E_ELEMENTNOTAVAILABLE;
			auto* owner = _root->ResolveControl(_owner, _ownerRuntimeId);
			if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
			const ControlWeakReference ownerLifetime(owner);
			auto* provider = _root->ProviderFor(owner);
			if (!provider) return E_OUTOFMEMORY;
			auto releaseProvider = [](ControlUiaProvider* value)
			{
				if (value) value->Release();
			};
			std::unique_ptr<ControlUiaProvider, decltype(releaseProvider)>
				providerLifetime(provider, releaseProvider);
			owner = ownerLifetime.Get();
			if (!owner
				|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner)
				return UIA_E_ELEMENTNOTAVAILABLE;
			*value = static_cast<IRawElementProviderSimple*>(provider);
			(void)providerLifetime.release();
			return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** value) override;
	HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* value) override;
	HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* value) override;
	HRESULT STDMETHODCALLTYPE ScrollIntoView() override
	{
		return PerformNodeAction(AutomationPattern::ScrollItem,
			[](AutomationPeer& source, uint32_t id)
			{ return source.ScrollAccessibilityVirtualNodeIntoView(id); });
	}
	HRESULT STDMETHODCALLTYPE Realize() override
	{
		return PerformNodeAction(AutomationPattern::VirtualizedItem,
			[](AutomationPeer& source, uint32_t id)
			{ return source.ScrollAccessibilityVirtualNodeIntoView(id); });
	}
	HRESULT STDMETHODCALLTYPE get_Row(int* value) override
	{
		return GetGridInt(value, [](const AccessibilityVirtualNode& node) { return node.Row; });
	}
	HRESULT STDMETHODCALLTYPE get_Column(int* value) override
	{
		return GetGridInt(value, [](const AccessibilityVirtualNode& node) { return node.Column; });
	}
	HRESULT STDMETHODCALLTYPE get_RowSpan(int* value) override
	{
		return GetGridInt(value, [](const AccessibilityVirtualNode& node) { return node.RowSpan; });
	}
	HRESULT STDMETHODCALLTYPE get_ColumnSpan(int* value) override
	{
		return GetGridInt(value, [](const AccessibilityVirtualNode& node) { return node.ColumnSpan; });
	}
	HRESULT STDMETHODCALLTYPE get_ContainingGrid(
		IRawElementProviderSimple** value) override
	{
		if (!value) return E_POINTER;
		*value = nullptr;
		try
		{
			if (!CurrentNode(nullptr) || !_root)
				return UIA_E_ELEMENTNOTAVAILABLE;
			auto* owner = _root->ResolveControl(_owner, _ownerRuntimeId);
			if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
			const ControlWeakReference ownerLifetime(owner);
			auto* provider = _root->ProviderFor(owner);
			if (!provider) return E_OUTOFMEMORY;
			auto releaseProvider = [](ControlUiaProvider* value)
			{
				if (value) value->Release();
			};
			std::unique_ptr<ControlUiaProvider, decltype(releaseProvider)>
				providerLifetime(provider, releaseProvider);
			owner = ownerLifetime.Get();
			if (!owner
				|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner)
				return UIA_E_ELEMENTNOTAVAILABLE;
			*value = static_cast<IRawElementProviderSimple*>(provider);
			(void)providerLifetime.release();
			return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	HRESULT STDMETHODCALLTYPE GetRowHeaderItems(SAFEARRAY** value) override;
	HRESULT STDMETHODCALLTYPE GetColumnHeaderItems(SAFEARRAY** value) override;

private:
	~VirtualUiaProvider()
	{
		if (_root)
		{
			_root->UnregisterVirtualProvider(_key, this);
			_root->Release();
		}
	}
	bool CurrentNode(AccessibilityVirtualNode* result,
		AutomationPeer** source = nullptr,
		std::shared_ptr<AutomationPeer>* sourceLease = nullptr) const
	{
		AccessibilityVirtualNode node;
		AutomationPeer* resolved = nullptr;
		if (!_root || !_root->ResolveVirtualNode(
			_owner, _ownerRuntimeId, _virtualId, node, &resolved,
			sourceLease)) return false;
		if (result) *result = std::move(node);
		if (source) *source = resolved;
		return true;
	}
	bool Supports(AutomationPattern pattern) const
	{
		AccessibilityVirtualNode node;
		return CurrentNode(&node)
			&& HasAutomationPattern(node.Patterns, pattern);
	}
	static HRESULT SetPatternAvailability(VARIANT* value,
		const AccessibilityVirtualNode& node,
		AutomationPattern pattern)
	{
		accessibility_detail::SetVariantBool(value,
			HasAutomationPattern(node.Patterns, pattern));
		return S_OK;
	}
	template<typename Action>
	HRESULT PerformNodeAction(
		AutomationPattern pattern, Action&& action)
	{
		try
		{
			AccessibilityVirtualNode node;
			AutomationPeer* source = nullptr;
			std::shared_ptr<AutomationPeer> sourceLease;
			if (!CurrentNode(&node, &source, &sourceLease) || !_root)
				return UIA_E_ELEMENTNOTAVAILABLE;
			if (!HasAutomationPattern(node.Patterns, pattern))
				return UIA_E_NOTSUPPORTED;
			if (!node.Enabled) return UIA_E_ELEMENTNOTENABLED;

			auto* owner = _root->ResolveControl(_owner, _ownerRuntimeId);
			if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
			const ControlWeakReference ownerLifetime(owner);
			const bool completed = action(*source, _virtualId);
			owner = ownerLifetime.Get();
			if (!owner
				|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner)
				return UIA_E_ELEMENTNOTAVAILABLE;
			return completed ? S_OK : UIA_E_INVALIDOPERATION;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	HRESULT SelectNode(AccessibilitySelectionAction action)
	{
		try
		{
			AccessibilityVirtualNode node;
			AutomationPeer* source = nullptr;
			std::shared_ptr<AutomationPeer> sourceLease;
			if (!CurrentNode(&node, &source, &sourceLease) || !_root)
				return UIA_E_ELEMENTNOTAVAILABLE;
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::SelectionItem))
				return UIA_E_NOTSUPPORTED;
			if (!node.Enabled) return UIA_E_ELEMENTNOTENABLED;

			auto* owner = _root->ResolveControl(_owner, _ownerRuntimeId);
			if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
			const ControlWeakReference ownerLifetime(owner);
			const auto container =
				source->GetAccessibilityVirtualContainerInfo();

			auto refresh = [&]()
			{
				owner = ownerLifetime.Get();
				return owner
					&& _root->ResolveControl(_owner, _ownerRuntimeId) == owner
					&& CurrentNode(&node, &source, &sourceLease);
			};
			if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;
			if (!HasAutomationPattern(
				node.Patterns, AutomationPattern::SelectionItem))
				return UIA_E_NOTSUPPORTED;
			if (!node.Enabled) return UIA_E_ELEMENTNOTENABLED;
			if (action == AccessibilitySelectionAction::Remove
				&& node.Selected && container.IsSelectionRequired)
				return UIA_E_INVALIDOPERATION;
			if (action == AccessibilitySelectionAction::Add
				&& !node.Selected && !container.CanSelectMultiple)
			{
				std::vector<uint32_t> selection;
				source->GetAccessibilityVirtualSelection(selection);
				if (!refresh()) return UIA_E_ELEMENTNOTAVAILABLE;
				if (!HasAutomationPattern(
					node.Patterns, AutomationPattern::SelectionItem))
					return UIA_E_NOTSUPPORTED;
				if (!node.Enabled) return UIA_E_ELEMENTNOTENABLED;
				if (!selection.empty()) return UIA_E_INVALIDOPERATION;
			}

			const bool completed = source->SelectAccessibilityVirtualNode(
				_virtualId, action);
			owner = ownerLifetime.Get();
			if (!owner
				|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner)
				return UIA_E_ELEMENTNOTAVAILABLE;
			return completed ? S_OK : UIA_E_INVALIDOPERATION;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return UIA_E_INVALIDOPERATION; }
	}
	HRESULT SetExpanded(bool expanded)
	{
		AccessibilityVirtualNode node;
		AutomationPeer* source = nullptr;
		std::shared_ptr<AutomationPeer> sourceLease;
		if (!CurrentNode(&node, &source, &sourceLease))
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::ExpandCollapse))
			return UIA_E_NOTSUPPORTED;
		if (!node.Enabled) return UIA_E_ELEMENTNOTENABLED;
		auto* owner = _root
			? _root->ResolveControl(_owner, _ownerRuntimeId) : nullptr;
		if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference ownerLifetime(owner);
		const bool changed = source->SetAccessibilityVirtualNodeExpanded(
			_virtualId, expanded);
		owner = ownerLifetime.Get();
		if (!owner || _root->ResolveControl(_owner, _ownerRuntimeId) != owner)
			return UIA_E_ELEMENTNOTAVAILABLE;
		return changed ? S_OK : UIA_E_INVALIDOPERATION;
	}
	template<typename Getter>
	HRESULT GetGridInt(int* value, Getter&& getter)
	{
		if (!value) return E_POINTER;
		AccessibilityVirtualNode node;
		if (!CurrentNode(&node)) return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(node.Patterns,
			AutomationPattern::GridItem)) return UIA_E_NOTSUPPORTED;
		*value = getter(node);
		return S_OK;
	}
	HRESULT CreateProviderArray(
		const std::vector<uint32_t>& ids, SAFEARRAY** value);

	std::atomic<ULONG> _references{ 1 };
	WindowUiaProvider* _root = nullptr;
	Control* _owner = nullptr;
	uint32_t _ownerRuntimeId = 0;
	uint32_t _virtualId = 0;
	uint64_t _key = 0;
};

Control* WindowUiaProvider::ResolveControl(Control* candidate) const noexcept
{
	auto* form = GetWindow();
	return form && Connected()
		? form->ResolveAccessibleControl(candidate) : nullptr;
}

Control* WindowUiaProvider::ResolveControl(
	Control* candidate, uint32_t runtimeId) const noexcept
{
	auto* resolved = ResolveControl(candidate);
	return resolved && runtimeId != 0
		&& resolved->GetAccessibilityRuntimeId() == runtimeId
		? resolved : nullptr;
}

ControlUiaProvider* WindowUiaProvider::ProviderFor(Control* control)
{
	control = ResolveControl(control);
	if (!control) return nullptr;
	const ControlWeakReference controlLifetime(control);
	const uint32_t runtimeId = control->GetAccessibilityRuntimeId();
	control = controlLifetime.Get();
	if (!control || ResolveControl(control, runtimeId) != control)
		return nullptr;
	std::lock_guard lock(_providerMutex);
	if (const auto position = _providers.find(runtimeId);
		position != _providers.end())
	{
		if (position->second && position->second->Matches(control))
		{
			position->second->AddRef();
			return position->second;
		}
		_providers.erase(position);
	}
	auto slot = _providers.end();
	try
	{
		auto [position, inserted] = _providers.emplace(runtimeId, nullptr);
		if (!inserted) return nullptr;
		slot = position;
	}
	catch (...)
	{
		return nullptr;
	}
	auto* provider = new (std::nothrow) ControlUiaProvider(this, control);
	if (!provider)
	{
		_providers.erase(slot);
		return nullptr;
	}
	slot->second = provider;
	return provider;
}

bool WindowUiaProvider::ResolveVirtualNode(
	Control* owner, uint32_t ownerRuntimeId, uint32_t virtualId,
	AccessibilityVirtualNode& node,
	AutomationPeer** source,
	std::shared_ptr<AutomationPeer>* sourceLease) const
{
	if (source) *source = nullptr;
	if (sourceLease) sourceLease->reset();
	try
	{
		auto* resolvedOwner = ResolveControl(owner, ownerRuntimeId);
		if (!resolvedOwner || virtualId == 0) return false;
		const ControlWeakReference ownerLifetime(resolvedOwner);
		auto lease = resolvedOwner->AcquireAutomationPeer();
		if (!lease) return false;
		auto* virtualized = lease.get();
		if (!virtualized->SupportsVirtualizedChildren()) return false;
		resolvedOwner = ownerLifetime.Get();
		if (!resolvedOwner
			|| ResolveControl(owner, ownerRuntimeId) != resolvedOwner)
			return false;
		if (!virtualized->TryGetAccessibilityVirtualNode(virtualId, node))
			return false;
		if (node.Id != virtualId) return false;

		// A virtual-node callback is framework-extensible. Re-resolve both the
		// Control lifetime and its Window membership before publishing a peer
		// pointer or allowing a provider to retain the raw owner identity.
		resolvedOwner = ownerLifetime.Get();
		if (!resolvedOwner
			|| ResolveControl(owner, ownerRuntimeId) != resolvedOwner)
			return false;
		lease = resolvedOwner->AcquireAutomationPeer();
		if (!lease) return false;
		virtualized = lease.get();
		if (!virtualized->SupportsVirtualizedChildren()) return false;
		if (source) *source = virtualized;
		if (sourceLease) *sourceLease = std::move(lease);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

VirtualUiaProvider* WindowUiaProvider::VirtualProviderFor(
	Control* owner, uint32_t virtualId)
{
	if (!owner || virtualId == 0) return nullptr;
	AccessibilityVirtualNode node;
	const uint32_t ownerRuntimeId = owner->GetAccessibilityRuntimeId();
	if (!ResolveVirtualNode(owner, ownerRuntimeId, virtualId, node)) return nullptr;
	return VirtualProviderForValidated(owner, ownerRuntimeId, virtualId);
}

VirtualUiaProvider* WindowUiaProvider::VirtualProviderForValidated(
	Control* owner, uint32_t ownerRuntimeId, uint32_t virtualId)
{
	if (!owner || ownerRuntimeId == 0 || virtualId == 0
		|| owner->GetAccessibilityRuntimeId() != ownerRuntimeId)
		return nullptr;
	const uint64_t key = (static_cast<uint64_t>(ownerRuntimeId) << 32)
		| virtualId;
	std::lock_guard lock(_providerMutex);
	if (const auto position = _virtualProviders.find(key);
		position != _virtualProviders.end())
	{
		if (position->second && position->second->Matches(owner, virtualId))
		{
			position->second->AddRef();
			return position->second;
		}
		_virtualProviders.erase(position);
	}
	auto slot = _virtualProviders.end();
	try
	{
		auto [position, inserted] = _virtualProviders.emplace(key, nullptr);
		if (!inserted) return nullptr;
		slot = position;
	}
	catch (...)
	{
		return nullptr;
	}
	auto* provider = new (std::nothrow) VirtualUiaProvider(
		this, owner, virtualId);
	if (!provider)
	{
		_virtualProviders.erase(slot);
		return nullptr;
	}
	slot->second = provider;
	return provider;
}

void WindowUiaProvider::UnregisterProvider(
	uint32_t runtimeId, ControlUiaProvider* provider)
{
	std::lock_guard lock(_providerMutex);
	const auto position = _providers.find(runtimeId);
	if (position != _providers.end() && position->second == provider)
		_providers.erase(position);
}

void WindowUiaProvider::UnregisterVirtualProvider(
	uint64_t key, VirtualUiaProvider* provider)
{
	std::lock_guard lock(_providerMutex);
	const auto position = _virtualProviders.find(key);
	if (position != _virtualProviders.end() && position->second == provider)
		_virtualProviders.erase(position);
}

void WindowUiaProvider::SetVirtualFocus(Control* owner, uint32_t virtualId)
{
	ClearVirtualFocus();
	if (!owner || virtualId == 0 || !HasNativeKeyboardFocus()) return;
	auto* form = GetWindow();
	if (!form) return;
	auto* focused = form->GetKeyboardFocusedElement();
	Control* authoritativeOwner = nullptr;
	uint32_t authoritativeId = 0;
	if (TryGetVirtualFocusedNode(
		focused, authoritativeOwner, authoritativeId))
	{
		// A realized peer (notably DataGridCell) is the authoritative focus
		// source. Do not leave an owner-level hint that can later resurrect it.
		return;
	}
	owner = ResolveControl(owner);
	if (!owner || form != GetWindow()
		|| form->GetKeyboardFocusedElement() != owner) return;
	std::lock_guard lock(_providerMutex);
	_focusedVirtualByOwner[owner->GetAccessibilityRuntimeId()] = virtualId;
}

void WindowUiaProvider::ClearVirtualFocus() noexcept
{
	try
	{
		std::lock_guard lock(_providerMutex);
		_focusedVirtualByOwner.clear();
	}
	catch (...) {}
}

bool WindowUiaProvider::IsVirtualFocused(
	Control* owner, uint32_t virtualId)
{
	if (!owner || virtualId == 0 || !HasNativeKeyboardFocus()) return false;
	auto* form = GetWindow();
	owner = ResolveControl(owner);
	if (!form || !owner || form != GetWindow()
		|| form->GetKeyboardFocusedElement() != owner) return false;
	std::lock_guard lock(_providerMutex);
	const auto position = _focusedVirtualByOwner.find(
		owner->GetAccessibilityRuntimeId());
	return position != _focusedVirtualByOwner.end()
		&& position->second == virtualId;
}

uint32_t WindowUiaProvider::VirtualFocusFor(Control* owner)
{
	if (!owner || !HasNativeKeyboardFocus()) return 0;
	auto* form = GetWindow();
	owner = ResolveControl(owner);
	if (!form || !owner || form != GetWindow()
		|| form->GetKeyboardFocusedElement() != owner) return 0;
	std::lock_guard lock(_providerMutex);
	const auto position = _focusedVirtualByOwner.find(
		owner->GetAccessibilityRuntimeId());
	return position == _focusedVirtualByOwner.end() ? 0 : position->second;
}

bool WindowUiaProvider::TryGetVirtualFocusedNode(
	Control* focused, Control*& owner, uint32_t& virtualId)
{
	owner = nullptr;
	virtualId = 0;
	if (!focused) return false;
	try
	{
		std::vector<ControlWeakReference> pending;
		pending.emplace_back(focused);
		std::unordered_set<Control*> visited;
		while (!pending.empty())
		{
			const ControlWeakReference candidateLifetime = pending.back();
			pending.pop_back();
			auto* candidate = candidateLifetime.Get();
			if (!candidate || !visited.insert(candidate).second) continue;
			const uint32_t runtimeId = candidate->GetAccessibilityRuntimeId();
			auto currentCandidate = [&]() -> Control*
			{
				candidate = candidateLifetime.Get();
				return candidate
					&& candidate->GetAccessibilityRuntimeId() == runtimeId
					&& candidate->GetPresentationWindow() == GetWindow()
					? candidate : nullptr;
			};
			auto source = candidate->AcquireAutomationPeer();
			candidate = currentCandidate();
			if (!source || !candidate) continue;
			uint32_t candidateId = 0;
			const bool supportsVirtualChildren =
				source->SupportsVirtualizedChildren();
			candidate = currentCandidate();
			if (!candidate) continue;
			const bool hasFocusedVirtualNode = supportsVirtualChildren
				&& source->TryGetAccessibilityVirtualFocusedNode(candidateId);
			candidate = currentCandidate();
			if (!candidate) continue;
			if (hasFocusedVirtualNode && candidateId != 0)
			{
				candidate = candidateLifetime.Get();
				AccessibilityVirtualNode node;
				if (candidate && ResolveVirtualNode(
						candidate, runtimeId, candidateId, node))
				{
					owner = candidate;
					virtualId = candidateId;
					return true;
				}
			}

			candidate = currentCandidate();
			if (!candidate) continue;
			auto* visualParent = candidate->GetVisualParent();
			auto* logicalParent = candidate->GetLogicalParent();
			auto* templatedParent = candidate->GetTemplatedParent();
			if (visualParent) pending.emplace_back(visualParent);
			if (logicalParent && logicalParent != visualParent)
				pending.emplace_back(logicalParent);
			if (templatedParent && templatedParent != visualParent
				&& templatedParent != logicalParent)
				pending.emplace_back(templatedParent);
		}
	}
	catch (...) {}
	return false;
}

std::vector<Control*> WindowUiaProvider::DirectChildren(Control* parent) const
{
	std::vector<Control*> result;
	auto* form = GetWindow();
	if (!form || !Connected()) return result;
	if (parent)
	{
		const uint32_t parentRuntimeId = parent->GetAccessibilityRuntimeId();
		const ControlWeakReference parentLifetime(parent);
		auto peer = parent->AcquireAutomationPeer();
		parent = parentLifetime.Get();
		if (!peer || !parent
			|| ResolveControl(parent, parentRuntimeId) != parent) return result;
		const bool virtualized = peer->SupportsVirtualizedChildren();
		parent = parentLifetime.Get();
		if (!parent || ResolveControl(parent, parentRuntimeId) != parent)
			return result;
		if (virtualized) return result;
	}
	const auto source = parent
		? parent->GetLogicalChildrenView()
		: form->GetLogicalChildrenView();
	std::vector<ControlWeakReference> snapshot;
	snapshot.reserve(source.size());
	for (auto* child : source)
		if (child) snapshot.emplace_back(child);
	std::vector<ControlWeakReference> resolvedChildren;
	resolvedChildren.reserve(snapshot.size());
	for (const auto& childLifetime : snapshot)
	{
		auto* child = childLifetime.Get();
		if (!child) continue;
		const uint32_t childRuntimeId = child->GetAccessibilityRuntimeId();
		auto* resolved = ResolveControl(child, childRuntimeId);
		child = childLifetime.Get();
		if (child && resolved == child)
			resolvedChildren.emplace_back(child);
	}
	result.reserve(resolvedChildren.size());
	for (const auto& childLifetime : resolvedChildren)
		if (auto* child = childLifetime.Get()) result.push_back(child);
	return result;
}

bool WindowUiaProvider::TryGetVirtualBoundaryChild(
	Control* owner, uint32_t parentId, bool last, uint32_t& result) const
{
	result = 0;
	if (!owner) return false;
	const uint32_t ownerRuntimeId = owner->GetAccessibilityRuntimeId();
	if (ResolveControl(owner, ownerRuntimeId) != owner) return false;
	const ControlWeakReference ownerLifetime(owner);
	auto source = owner->AcquireAutomationPeer();
	owner = ownerLifetime.Get();
	if (!source || !owner || ResolveControl(owner, ownerRuntimeId) != owner)
		return false;
	if (!source->SupportsVirtualizedChildren()) return false;
	owner = ownerLifetime.Get();
	if (!owner || ResolveControl(owner, ownerRuntimeId) != owner) return false;
	const size_t count = source->GetAccessibilityVirtualChildCount(parentId);
	owner = ownerLifetime.Get();
	if (!owner || ResolveControl(owner, ownerRuntimeId) != owner) return false;
	for (size_t offset = 0; offset < count; ++offset)
	{
		const size_t index = last ? count - offset - 1 : offset;
		uint32_t candidate = 0;
		const bool found = source->TryGetAccessibilityVirtualChildAt(
			parentId, index, candidate);
		owner = ownerLifetime.Get();
		if (!owner || ResolveControl(owner, ownerRuntimeId) != owner)
			return false;
		if (!found || candidate == 0) continue;
		AccessibilityVirtualNode node;
		if (!ResolveVirtualNode(owner, ownerRuntimeId,
			candidate, node)) continue;
		result = candidate;
		return true;
	}
	return false;
}

bool WindowUiaProvider::TryGetVirtualSibling(
	Control* owner, uint32_t parentId, uint32_t id,
	bool next, uint32_t& result) const
{
	result = 0;
	if (!owner) return false;
	const uint32_t ownerRuntimeId = owner->GetAccessibilityRuntimeId();
	if (ResolveControl(owner, ownerRuntimeId) != owner) return false;
	const ControlWeakReference ownerLifetime(owner);
	auto source = owner->AcquireAutomationPeer();
	owner = ownerLifetime.Get();
	if (!source || !owner || ResolveControl(owner, ownerRuntimeId) != owner)
		return false;
	if (!source->SupportsVirtualizedChildren()) return false;
	owner = ownerLifetime.Get();
	if (!owner || ResolveControl(owner, ownerRuntimeId) != owner) return false;
	const bool found = source->TryGetAccessibilityVirtualSibling(
		parentId, id, next, result);
	owner = ownerLifetime.Get();
	if (!owner || ResolveControl(owner, ownerRuntimeId) != owner) return false;
	if (!found || result == 0) return false;
	AccessibilityVirtualNode node;
	if (!ResolveVirtualNode(owner, ownerRuntimeId, result, node))
	{
		result = 0;
		return false;
	}
	return true;
}

Control* WindowUiaProvider::ParentOf(Control* control) const
{
	if (!control) return nullptr;
	const ControlWeakReference controlLifetime(control);
	const uint32_t runtimeId = control->GetAccessibilityRuntimeId();
	auto* resolved = ResolveControl(control, runtimeId);
	control = controlLifetime.Get();
	if (!control || resolved != control) return nullptr;
	return control->GetLogicalParent();
}

Control* WindowUiaProvider::SiblingOf(Control* control, bool next) const
{
	if (!control) return nullptr;
	const ControlWeakReference controlLifetime(control);
	auto* parent = control->GetLogicalParent();
	const auto siblings = DirectChildren(parent);
	control = controlLifetime.Get();
	if (!control) return nullptr;
	const auto position = std::find(siblings.begin(), siblings.end(), control);
	if (position == siblings.end()) return nullptr;
	if (next)
		return position + 1 != siblings.end() ? *(position + 1) : nullptr;
	return position != siblings.begin() ? *(position - 1) : nullptr;
}

HRESULT WindowUiaProvider::Navigate(NavigateDirection direction,
	IRawElementProviderFragment** value) try
{
	if (!value) return E_POINTER;
	*value = nullptr;
	if (!Connected()) return UIA_E_ELEMENTNOTAVAILABLE;
	if (direction != NavigateDirection_FirstChild
		&& direction != NavigateDirection_LastChild) return S_OK;
	const auto children = DirectChildren(nullptr);
	if (children.empty()) return S_OK;
	auto* provider = ProviderFor(direction == NavigateDirection_FirstChild
		? children.front() : children.back());
	if (!provider) return E_OUTOFMEMORY;
	*value = static_cast<IRawElementProviderFragment*>(provider);
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT WindowUiaProvider::ElementProviderFromPoint(double x, double y,
	IRawElementProviderFragment** value) try
{
	if (!value) return E_POINTER;
	*value = nullptr;
	auto* form = GetWindow();
	if (!form || !Connected()) return UIA_E_ELEMENTNOTAVAILABLE;
	POINT clientPoint{ static_cast<LONG>(std::floor(x)),
		static_cast<LONG>(std::floor(y)) };
	if (!::ScreenToClient(form->Handle, &clientPoint))
	{
		const DWORD error = ::GetLastError();
		return HRESULT_FROM_WIN32(error != ERROR_SUCCESS
			? error : ERROR_INVALID_WINDOW_HANDLE);
	}
	float dpiScale = form->GetDpiScale();
	if (dpiScale <= 0.0f) dpiScale = 1.0f;
	const float contentX = static_cast<float>(clientPoint.x) / dpiScale;
	const float contentY = static_cast<float>(clientPoint.y
		- form->GetTitleBarHeightPixels()) / dpiScale;
	auto controls = form->GetAccessibleControls();
	std::vector<ControlWeakReference> controlSnapshot;
	controlSnapshot.reserve(controls.size());
	for (auto* control : controls)
		if (control) controlSnapshot.emplace_back(control);
	for (auto position = controlSnapshot.rbegin();
		position != controlSnapshot.rend(); ++position)
	{
		auto* control = position->Get();
		if (!control) continue;
		const uint32_t controlRuntimeId = control->GetAccessibilityRuntimeId();
		const ControlWeakReference controlLifetime(control);
		const bool visible = control->GetAccessibilitySnapshot().Visible;
		control = controlLifetime.Get();
		if (!visible || !control
			|| ResolveControl(control, controlRuntimeId) != control) continue;
		auto source = control->AcquireAutomationPeer();
		control = controlLifetime.Get();
		if (!source || !control
			|| ResolveControl(control, controlRuntimeId) != control) continue;
		if (source->SupportsVirtualizedChildren())
		{
			control = controlLifetime.Get();
			if (!control || ResolveControl(control, controlRuntimeId) != control)
				continue;
			D2D1_POINT_2F local{};
			uint32_t virtualId = 0;
			if (control->TryTransformRenderPointToLocal(
				D2D1::Point2F(contentX, contentY), local)
				&& control->IsRenderPointInsideClip(
					D2D1::Point2F(contentX, contentY))
				&& source->TryHitTestAccessibilityVirtualNode(
					local.x, local.y, virtualId)
				&& virtualId != 0)
			{
				AccessibilityVirtualNode node;
				control = controlLifetime.Get();
				if (control && ResolveControl(control, controlRuntimeId) == control
					&& ResolveVirtualNode(control,
						controlRuntimeId, virtualId, node))
				{
					auto* provider = VirtualProviderFor(control, virtualId);
					if (!provider) return E_OUTOFMEMORY;
					*value = static_cast<IRawElementProviderFragment*>(provider);
					return S_OK;
				}
			}
		}
		control = controlLifetime.Get();
		if (!control || ResolveControl(control, controlRuntimeId) != control)
			continue;
		D2D1_POINT_2F local{};
		if (!control->TryTransformRenderPointToLocal(
			D2D1::Point2F(contentX, contentY), local)
			|| !control->IsRenderPointInsideClip(
				D2D1::Point2F(contentX, contentY))
			|| !control->ContainsPoint(
				static_cast<int>(std::floor(local.x)),
				static_cast<int>(std::floor(local.y)))) continue;
		auto* provider = ProviderFor(control);
		if (!provider) return E_OUTOFMEMORY;
		*value = static_cast<IRawElementProviderFragment*>(provider);
		return S_OK;
	}
	*value = static_cast<IRawElementProviderFragment*>(this);
	AddRef();
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT WindowUiaProvider::GetFocus(IRawElementProviderFragment** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	try
	{
		auto* form = GetWindow();
		if (!form || !Connected()) return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasNativeKeyboardFocus()) return S_OK;
		const ControlWeakReference formLifetime(form);
		auto* focusedControl = form->GetKeyboardFocusedElement();
		if (!focusedControl)
		{
			*value = static_cast<IRawElementProviderFragment*>(this);
			AddRef();
			return S_OK;
		}
		const ControlWeakReference focusedLifetime(focusedControl);
		Control* virtualOwner = nullptr;
		uint32_t virtualId = 0;
		if (TryGetVirtualFocusedNode(
			focusedControl, virtualOwner, virtualId))
		{
			auto* provider = VirtualProviderFor(virtualOwner, virtualId);
			if (!provider) return E_OUTOFMEMORY;
			form = dynamic_cast<Window*>(formLifetime.Get());
			if (!form || !Connected() || !HasNativeKeyboardFocus()
				|| form->GetKeyboardFocusedElement() != focusedLifetime.Get())
			{
				provider->Release();
				return form ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
			}
			*value = static_cast<IRawElementProviderFragment*>(provider);
			return S_OK;
		}
		if (const uint32_t focused = VirtualFocusFor(focusedControl);
			focused != 0)
		{
			if (auto* provider = VirtualProviderFor(focusedControl, focused))
			{
				*value = static_cast<IRawElementProviderFragment*>(provider);
				return S_OK;
			}
		}
		auto* provider = ProviderFor(focusedControl);
		if (!provider) return E_OUTOFMEMORY;
		*value = static_cast<IRawElementProviderFragment*>(provider);
		return S_OK;
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

HRESULT ControlUiaProvider::Navigate(NavigateDirection direction,
	IRawElementProviderFragment** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	try
	{
	auto* control = CurrentControl();
	if (!control || !_root) return UIA_E_ELEMENTNOTAVAILABLE;
	const ControlWeakReference controlLifetime(control);
	auto currentControl = [&]() -> Control*
	{
		control = controlLifetime.Get();
		return control && CurrentControl() == control ? control : nullptr;
	};
	Control* destination = nullptr;
	uint32_t virtualDestination = 0;
	Control* virtualOwner = nullptr;
	switch (direction)
	{
	case NavigateDirection_Parent:
		destination = _root->ParentOf(control);
		if (!destination)
		{
			*value = static_cast<IRawElementProviderFragment*>(_root);
			_root->AddRef();
			return S_OK;
		}
		break;
	case NavigateDirection_FirstChild:
		{
			const auto children = _root->DirectChildren(control);
			if (!children.empty())
			{
				destination = children.front();
				const ControlWeakReference destinationLifetime(destination);
				control = currentControl();
				destination = destinationLifetime.Get();
				if (!control || !destination)
					return UIA_E_ELEMENTNOTAVAILABLE;
			}
			else
			{
				control = controlLifetime.Get();
				if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
				if (_root->TryGetVirtualBoundaryChild(
					control, 0, false, virtualDestination))
				{
					virtualOwner = control;
				}
			}
		}
		break;
	case NavigateDirection_LastChild:
		if (_root->TryGetVirtualBoundaryChild(
			control, 0, true, virtualDestination))
		{
			virtualOwner = control;
		}
		else
		{
			control = controlLifetime.Get();
			if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
			const auto children = _root->DirectChildren(control);
			if (!children.empty()) destination = children.back();
		}
		break;
	case NavigateDirection_NextSibling:
		{
			destination = _root->SiblingOf(control, true);
			const ControlWeakReference destinationLifetime(destination);
			control = currentControl();
			destination = destinationLifetime.Get();
			if (!control)
				return UIA_E_ELEMENTNOTAVAILABLE;
			auto* parent = control->GetLogicalParent();
			if (!destination && parent && _root->TryGetVirtualBoundaryChild(
				parent, 0, false, virtualDestination))
			{
				virtualOwner = parent;
			}
		}
		break;
	case NavigateDirection_PreviousSibling:
		destination = _root->SiblingOf(control, false);
		{
			const ControlWeakReference destinationLifetime(destination);
			control = currentControl();
			destination = destinationLifetime.Get();
		}
		if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
		break;
	default:
		return E_INVALIDARG;
	}
	if (virtualDestination != 0)
	{
		auto* provider = _root->VirtualProviderFor(
			virtualOwner, virtualDestination);
		if (!provider) return E_OUTOFMEMORY;
		*value = static_cast<IRawElementProviderFragment*>(provider);
		return S_OK;
	}
	if (!destination) return S_OK;
	auto* provider = _root->ProviderFor(destination);
	if (!provider) return E_OUTOFMEMORY;
	*value = static_cast<IRawElementProviderFragment*>(provider);
	return S_OK;
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

HRESULT VirtualUiaProvider::Navigate(NavigateDirection direction,
	IRawElementProviderFragment** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	try
	{
	AccessibilityVirtualNode node;
	if (!CurrentNode(&node) || !_root || !_owner)
		return UIA_E_ELEMENTNOTAVAILABLE;
	auto* owner = _root->ResolveControl(_owner, _ownerRuntimeId);
	if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
	const ControlWeakReference ownerLifetime(owner);
	auto currentOwner = [&]() -> Control*
	{
		owner = ownerLifetime.Get();
		return owner && _root->ResolveControl(_owner, _ownerRuntimeId) == owner
			? owner : nullptr;
	};
	auto returnVirtual = [&](uint32_t id) -> HRESULT
	{
		if (id == 0) return S_OK;
		auto* liveOwner = currentOwner();
		if (!liveOwner) return UIA_E_ELEMENTNOTAVAILABLE;
		auto* provider = _root->VirtualProviderFor(liveOwner, id);
		if (!provider) return E_OUTOFMEMORY;
		*value = static_cast<IRawElementProviderFragment*>(provider);
		return S_OK;
	};
	switch (direction)
	{
	case NavigateDirection_Parent:
		if (node.ParentId != 0) return returnVirtual(node.ParentId);
		{
			auto* liveOwner = currentOwner();
			if (!liveOwner) return UIA_E_ELEMENTNOTAVAILABLE;
			auto* provider = _root->ProviderFor(liveOwner);
			if (!provider) return E_OUTOFMEMORY;
			*value = static_cast<IRawElementProviderFragment*>(provider);
			return S_OK;
		}
	case NavigateDirection_FirstChild:
	case NavigateDirection_LastChild:
		{
			uint32_t child = 0;
			auto* liveOwner = currentOwner();
			if (!liveOwner) return UIA_E_ELEMENTNOTAVAILABLE;
			if (!_root->TryGetVirtualBoundaryChild(liveOwner, _virtualId,
				direction == NavigateDirection_LastChild, child))
				return currentOwner() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
			return returnVirtual(child);
		}
	case NavigateDirection_NextSibling:
	case NavigateDirection_PreviousSibling:
		{
			uint32_t sibling = 0;
			auto* liveOwner = currentOwner();
			if (!liveOwner) return UIA_E_ELEMENTNOTAVAILABLE;
			if (_root->TryGetVirtualSibling(liveOwner, node.ParentId, _virtualId,
				direction == NavigateDirection_NextSibling, sibling))
				return returnVirtual(sibling);
			liveOwner = currentOwner();
			if (!liveOwner) return UIA_E_ELEMENTNOTAVAILABLE;
			if (direction == NavigateDirection_NextSibling) return S_OK;
			if (node.ParentId == 0)
			{
				const auto controls = _root->DirectChildren(liveOwner);
				if (!currentOwner()) return UIA_E_ELEMENTNOTAVAILABLE;
				if (!controls.empty())
				{
					auto* provider = _root->ProviderFor(controls.back());
					if (!provider) return E_OUTOFMEMORY;
					*value = static_cast<IRawElementProviderFragment*>(provider);
				}
			}
			return S_OK;
		}
	default:
		return E_INVALIDARG;
	}
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

HRESULT VirtualUiaProvider::GetSelection(SAFEARRAY** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	try
	{
		AccessibilityVirtualNode node;
		AutomationPeer* source = nullptr;
		std::shared_ptr<AutomationPeer> sourceLease;
		if (!CurrentNode(&node, &source, &sourceLease) || !_root)
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::Selection))
			return UIA_E_NOTSUPPORTED;

		auto* owner = _root->ResolveControl(_owner, _ownerRuntimeId);
		if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference ownerLifetime(owner);

		auto refresh = [&]() -> HRESULT
		{
			owner = ownerLifetime.Get();
			if (!owner
				|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner
				|| !CurrentNode(&node, &source, &sourceLease))
				return UIA_E_ELEMENTNOTAVAILABLE;
			return HasAutomationPattern(
				node.Patterns, AutomationPattern::Selection)
				? S_OK : UIA_E_NOTSUPPORTED;
		};
		HRESULT refreshed = refresh();
		if (FAILED(refreshed)) return refreshed;
		const size_t childCount =
			source->GetAccessibilityVirtualChildCount(_virtualId);
		refreshed = refresh();
		if (FAILED(refreshed)) return refreshed;
		std::vector<uint32_t> children;
		children.reserve(childCount);
		for (size_t index = 0; index < childCount; ++index)
		{
			uint32_t childId = 0;
			const bool found = source->TryGetAccessibilityVirtualChildAt(
				_virtualId, index, childId);
			refreshed = refresh();
			if (FAILED(refreshed)) return refreshed;
			if (found && childId != 0) children.push_back(childId);
		}

		std::vector<uint32_t> selection;
		selection.reserve(children.size());
		for (const uint32_t childId : children)
		{
			if (childId == 0) continue;
			owner = ownerLifetime.Get();
			if (!owner
				|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner)
				return UIA_E_ELEMENTNOTAVAILABLE;

			AccessibilityVirtualNode child;
			if (!_root->ResolveVirtualNode(
				owner, _ownerRuntimeId, childId, child))
			{
				refreshed = refresh();
				if (FAILED(refreshed)) return refreshed;
				continue;
			}
			refreshed = refresh();
			if (FAILED(refreshed)) return refreshed;
			if (child.ParentId == _virtualId
				&& HasAutomationPattern(
					child.Patterns, AutomationPattern::GridItem)
				&& child.Selected)
				selection.push_back(childId);
		}
		refreshed = refresh();
		if (FAILED(refreshed)) return refreshed;
		if (selection.empty() && node.SelectionReturnsNullWhenEmpty)
			return S_OK;
		return CreateProviderArray(selection, value);
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

HRESULT VirtualUiaProvider::get_CanSelectMultiple(BOOL* value)
{
	if (!value) return E_POINTER;
	*value = FALSE;
	try
	{
		AccessibilityVirtualNode node;
		AutomationPeer* source = nullptr;
		std::shared_ptr<AutomationPeer> sourceLease;
		if (!CurrentNode(&node, &source, &sourceLease) || !_root)
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::Selection))
			return UIA_E_NOTSUPPORTED;

		auto* owner = _root->ResolveControl(_owner, _ownerRuntimeId);
		if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference ownerLifetime(owner);
		const auto container =
			source->GetAccessibilityVirtualContainerInfo();
		owner = ownerLifetime.Get();
		if (!owner
			|| _root->ResolveControl(_owner, _ownerRuntimeId) != owner
			|| !CurrentNode(&node, &source, &sourceLease))
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::Selection)
			|| !HasAutomationPattern(
				container.Patterns, AutomationPattern::Selection))
			return UIA_E_NOTSUPPORTED;
		*value = container.CanSelectMultiple ? TRUE : FALSE;
		return S_OK;
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

HRESULT VirtualUiaProvider::get_IsSelectionRequired(BOOL* value)
{
	if (!value) return E_POINTER;
	*value = FALSE;
	try
	{
		AccessibilityVirtualNode node;
		if (!CurrentNode(&node)) return UIA_E_ELEMENTNOTAVAILABLE;
		return HasAutomationPattern(
			node.Patterns, AutomationPattern::Selection)
			? S_OK : UIA_E_NOTSUPPORTED;
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

HRESULT ControlUiaProvider::GetSelection(SAFEARRAY** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	try
	{
		auto* control = CurrentControl();
		if (!control || !_root) return UIA_E_ELEMENTNOTAVAILABLE;
		// A peer can advertise Selection both at its root and through its
		// virtual container (Calendar, ComboBox and DataGrid all do). The
		// virtual projection is authoritative because it can return every
		// selected logical item rather than only one realized Control.
		bool supportsVirtualizedChildren = false;
		HRESULT queryResult = PerformPeerQuery(
			[&supportsVirtualizedChildren](AutomationPeer& peer) -> HRESULT
		{
			supportsVirtualizedChildren = peer.SupportsVirtualizedChildren();
			return S_OK;
		});
		if (FAILED(queryResult)) return queryResult;
		if (supportsVirtualizedChildren)
		{
			AccessibilityVirtualContainerInfo info;
			queryResult = PerformPeerQuery(
				[&info](AutomationPeer& peer) -> HRESULT
			{
				info = peer.GetAccessibilityVirtualContainerInfo();
				return S_OK;
			});
			if (FAILED(queryResult)) return queryResult;
			if (HasAutomationPattern(
				info.Patterns, AutomationPattern::Selection))
			{
				std::vector<uint32_t> selection;
				queryResult = PerformPeerQuery(
					[&selection](AutomationPeer& peer) -> HRESULT
				{
					peer.GetAccessibilityVirtualSelection(selection);
					return S_OK;
				});
				if (FAILED(queryResult)) return queryResult;
				return CreateVirtualProviderArray(selection, value);
			}
		}

		bool supportsSelection = false;
		queryResult = PerformPeerQuery(
			[&supportsSelection](AutomationPeer& peer) -> HRESULT
		{
			supportsSelection = peer.SupportsPattern(
				AutomationPattern::Selection);
			return S_OK;
		});
		if (FAILED(queryResult)) return queryResult;
		if (!supportsSelection)
			return UIA_E_NOTSUPPORTED;
		Control* selected = nullptr;
		queryResult = PerformPeerQuery(
			[&selected](AutomationPeer& peer) -> HRESULT
		{
			selected = peer.GetSelectedItem();
			return S_OK;
		});
		if (FAILED(queryResult)) return queryResult;
		if (selected)
		{
			selected = _root->ResolveControl(selected);
			if (!selected) return UIA_E_ELEMENTNOTAVAILABLE;
			const uint32_t selectedRuntimeId =
				selected->GetAccessibilityRuntimeId();
			if (_root->ResolveControl(selected, selectedRuntimeId) != selected)
				return UIA_E_ELEMENTNOTAVAILABLE;
		}
		accessibility_detail::UniqueSafeArray result(
			::SafeArrayCreateVector(VT_UNKNOWN, 0, selected ? 1 : 0));
		if (!result) return E_OUTOFMEMORY;
		if (selected)
		{
			auto* provider = _root->ProviderFor(selected);
			if (!provider) return E_OUTOFMEMORY;
			LONG index = 0;
			IUnknown* unknown = static_cast<IRawElementProviderSimple*>(provider);
			const HRESULT hr = ::SafeArrayPutElement(
				result.get(), &index, unknown);
			provider->Release();
			if (FAILED(hr)) return hr;
		}
		*value = result.release();
		return S_OK;
	}
	catch (const std::bad_alloc&)
	{
		return E_OUTOFMEMORY;
	}
	catch (...)
	{
		return UIA_E_INVALIDOPERATION;
	}
}

HRESULT ControlUiaProvider::CreateVirtualProviderArray(
	const std::vector<uint32_t>& ids, SAFEARRAY** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	if (!_root) return UIA_E_ELEMENTNOTAVAILABLE;
	if (ids.size() > static_cast<size_t>(LONG_MAX))
		return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);

	accessibility_detail::UniqueSafeArray result(
		::SafeArrayCreateVector(
			VT_UNKNOWN, 0, static_cast<ULONG>(ids.size())));
	if (!result) return E_OUTOFMEMORY;
	try
	{
		auto* control = CurrentControl();
		if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference controlLifetime(control);
		const uint32_t ownerRuntimeId = control->GetAccessibilityRuntimeId();
		if (ownerRuntimeId == 0) return UIA_E_ELEMENTNOTAVAILABLE;

		for (size_t position = 0; position < ids.size(); ++position)
		{
			if (ids[position] == 0) return UIA_E_ELEMENTNOTAVAILABLE;
			// The peer produced and atomically validated this ID set immediately
			// before this call.  Register providers without resolving the whole
			// accessible tree and re-reading every virtual node; the owner is
			// validated once before and once after the allocation-only loop.
			auto* provider = _root->VirtualProviderForValidated(
				control, ownerRuntimeId, ids[position]);
			if (!provider)
				return controlLifetime.Get() == control
					? E_OUTOFMEMORY : UIA_E_ELEMENTNOTAVAILABLE;
			LONG index = static_cast<LONG>(position);
			IUnknown* unknown =
				static_cast<IRawElementProviderSimple*>(provider);
			const HRESULT hr = ::SafeArrayPutElement(
				result.get(), &index, unknown);
			provider->Release();
			if (FAILED(hr)) return hr;
		}
		control = controlLifetime.Get();
		if (!control || control->GetAccessibilityRuntimeId() != ownerRuntimeId
			|| CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
	}
	catch (const std::bad_alloc&)
	{
		return E_OUTOFMEMORY;
	}
	catch (...)
	{
		return UIA_E_INVALIDOPERATION;
	}
	*value = result.release();
	return S_OK;
}

HRESULT ControlUiaProvider::GetItem(
	int row, int column, IRawElementProviderSimple** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	try
	{
		auto* control = CurrentControl();
		if (!control || !_root) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference controlLifetime(control);
		auto source = VirtualSourceLease();
		if (!source) return UIA_E_NOTSUPPORTED;
		const auto container = source->GetAccessibilityVirtualContainerInfo();

		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			container.Patterns, AutomationPattern::Grid))
			return UIA_E_NOTSUPPORTED;
		source = VirtualSourceLease();
		if (!source) return UIA_E_NOTSUPPORTED;

		uint32_t id = 0;
		if (!source->GetAccessibilityVirtualItemAt(row, column, id)
			|| id == 0)
		{
			control = controlLifetime.Get();
			return control && CurrentControl() == control
				? E_INVALIDARG : UIA_E_ELEMENTNOTAVAILABLE;
		}
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		AccessibilityVirtualNode node;
		if (!_root->ResolveVirtualNode(
			control, _runtimeId, id, node))
			return UIA_E_ELEMENTNOTAVAILABLE;
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		auto* provider = _root->VirtualProviderFor(control, id);
		if (!provider)
			return CurrentControl() == control
				? E_OUTOFMEMORY : UIA_E_ELEMENTNOTAVAILABLE;
		*value = static_cast<IRawElementProviderSimple*>(provider);
		return S_OK;
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

HRESULT ControlUiaProvider::get_RowCount(int* value) try
{
	if (!value) return E_POINTER;
	AccessibilityVirtualContainerInfo info;
	const HRESULT result = TryGetVirtualContainerInfo(info);
	if (FAILED(result)) return result;
	if (!HasAutomationPattern(info.Patterns, AutomationPattern::Grid))
		return UIA_E_NOTSUPPORTED;
	*value = info.RowCount;
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT ControlUiaProvider::get_ColumnCount(int* value) try
{
	if (!value) return E_POINTER;
	AccessibilityVirtualContainerInfo info;
	const HRESULT result = TryGetVirtualContainerInfo(info);
	if (FAILED(result)) return result;
	if (!HasAutomationPattern(info.Patterns, AutomationPattern::Grid))
		return UIA_E_NOTSUPPORTED;
	*value = info.ColumnCount;
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT ControlUiaProvider::GetRowHeaders(SAFEARRAY** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	try
	{
		auto* control = CurrentControl();
		if (!control || !_root) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference controlLifetime(control);
		auto source = VirtualSourceLease();
		if (!source) return UIA_E_NOTSUPPORTED;
		const auto info = source->GetAccessibilityVirtualContainerInfo();
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(info.Patterns, AutomationPattern::Table))
			return UIA_E_NOTSUPPORTED;
		std::vector<uint32_t> headers;
		source->GetAccessibilityVirtualRowHeaders(headers);
		// WPF's root table projection enumerates only row-header peers that
		// currently exist; the source already returns this compact set.
		headers.erase(std::remove(headers.begin(), headers.end(), 0),
			headers.end());
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		return CreateVirtualProviderArray(headers, value);
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

HRESULT ControlUiaProvider::GetColumnHeaders(SAFEARRAY** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	try
	{
		auto* control = CurrentControl();
		if (!control || !_root) return UIA_E_ELEMENTNOTAVAILABLE;
		const ControlWeakReference controlLifetime(control);
		auto source = VirtualSourceLease();
		if (!source) return UIA_E_NOTSUPPORTED;
		const auto info = source->GetAccessibilityVirtualContainerInfo();
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(info.Patterns, AutomationPattern::Table))
			return UIA_E_NOTSUPPORTED;
		std::vector<uint32_t> headers;
		source->GetAccessibilityVirtualColumnHeaders(headers);
		headers.erase(std::remove(headers.begin(), headers.end(), 0),
			headers.end());
		control = controlLifetime.Get();
		if (!control || CurrentControl() != control)
			return UIA_E_ELEMENTNOTAVAILABLE;
		return CreateVirtualProviderArray(headers, value);
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

HRESULT ControlUiaProvider::get_RowOrColumnMajor(
	RowOrColumnMajor* value) try
{
	if (!value) return E_POINTER;
	if (!CurrentControl()) return UIA_E_ELEMENTNOTAVAILABLE;
	if (!SupportsTable()) return UIA_E_NOTSUPPORTED;
	*value = RowOrColumnMajor_RowMajor;
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT ControlUiaProvider::Scroll(
	ScrollAmount horizontalAmount, ScrollAmount verticalAmount) try
{
	auto* control = CurrentControl();
	if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
	const ControlWeakReference controlLifetime(control);
	auto source = VirtualSourceLease();
	if (!source) return UIA_E_NOTSUPPORTED;
	const auto container = source->GetAccessibilityVirtualContainerInfo();
	control = controlLifetime.Get();
	if (!control || CurrentControl() != control)
		return UIA_E_ELEMENTNOTAVAILABLE;
	if (!HasAutomationPattern(container.Patterns, AutomationPattern::Scroll))
		return UIA_E_NOTSUPPORTED;
	const bool enabled = control->GetAccessibilitySnapshot().Enabled;
	control = controlLifetime.Get();
	if (!control || CurrentControl() != control)
		return UIA_E_ELEMENTNOTAVAILABLE;
	if (!enabled)
		return UIA_E_ELEMENTNOTENABLED;
	auto convert = [](ScrollAmount amount,
		AccessibilityScrollAmount& result) -> bool
	{
		switch (amount)
		{
		case ScrollAmount_LargeDecrement:
			result = AccessibilityScrollAmount::LargeDecrement; return true;
		case ScrollAmount_SmallDecrement:
			result = AccessibilityScrollAmount::SmallDecrement; return true;
		case ScrollAmount_NoAmount:
			result = AccessibilityScrollAmount::NoAmount; return true;
		case ScrollAmount_LargeIncrement:
			result = AccessibilityScrollAmount::LargeIncrement; return true;
		case ScrollAmount_SmallIncrement:
			result = AccessibilityScrollAmount::SmallIncrement; return true;
		default: return false;
		}
	};
	AccessibilityScrollAmount horizontal{};
	AccessibilityScrollAmount vertical{};
	if (!convert(horizontalAmount, horizontal)
		|| !convert(verticalAmount, vertical)) return E_INVALIDARG;
	AccessibilityScrollInfo info;
	if (!source->GetAccessibilityScrollInfo(info))
		return UIA_E_INVALIDOPERATION;
	control = controlLifetime.Get();
	if (!control || CurrentControl() != control)
		return UIA_E_ELEMENTNOTAVAILABLE;
	if ((!info.HorizontallyScrollable
			&& horizontal != AccessibilityScrollAmount::NoAmount)
		|| (!info.VerticallyScrollable
			&& vertical != AccessibilityScrollAmount::NoAmount))
		return UIA_E_INVALIDOPERATION;
	const bool scrolled = source->ScrollAccessibility(horizontal, vertical);
	control = controlLifetime.Get();
	if (!control || CurrentControl() != control)
		return UIA_E_ELEMENTNOTAVAILABLE;
	return scrolled ? S_OK : UIA_E_INVALIDOPERATION;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT ControlUiaProvider::SetScrollPercent(
	double horizontalPercent, double verticalPercent) try
{
	auto* control = CurrentControl();
	if (!control) return UIA_E_ELEMENTNOTAVAILABLE;
	const ControlWeakReference controlLifetime(control);
	auto source = VirtualSourceLease();
	if (!source) return UIA_E_NOTSUPPORTED;
	const auto container = source->GetAccessibilityVirtualContainerInfo();
	control = controlLifetime.Get();
	if (!control || CurrentControl() != control)
		return UIA_E_ELEMENTNOTAVAILABLE;
	if (!HasAutomationPattern(container.Patterns, AutomationPattern::Scroll))
		return UIA_E_NOTSUPPORTED;
	const bool enabled = control->GetAccessibilitySnapshot().Enabled;
	control = controlLifetime.Get();
	if (!control || CurrentControl() != control)
		return UIA_E_ELEMENTNOTAVAILABLE;
	if (!enabled)
		return UIA_E_ELEMENTNOTENABLED;
	auto valid = [](double value)
	{
		return value == UIA_ScrollPatternNoScroll
			|| (std::isfinite(value) && value >= 0.0 && value <= 100.0);
	};
	if (!valid(horizontalPercent) || !valid(verticalPercent))
		return E_INVALIDARG;
	AccessibilityScrollInfo info;
	if (!source->GetAccessibilityScrollInfo(info))
		return UIA_E_INVALIDOPERATION;
	control = controlLifetime.Get();
	if (!control || CurrentControl() != control)
		return UIA_E_ELEMENTNOTAVAILABLE;
	if ((!info.HorizontallyScrollable
			&& horizontalPercent != UIA_ScrollPatternNoScroll)
		|| (!info.VerticallyScrollable
			&& verticalPercent != UIA_ScrollPatternNoScroll))
		return UIA_E_INVALIDOPERATION;
	const bool scrolled = source->SetAccessibilityScrollPercent(
		horizontalPercent, verticalPercent);
	control = controlLifetime.Get();
	if (!control || CurrentControl() != control)
		return UIA_E_ELEMENTNOTAVAILABLE;
	return scrolled ? S_OK : UIA_E_INVALIDOPERATION;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT ControlUiaProvider::get_HorizontalScrollPercent(double* value) try
{
	if (!value) return E_POINTER;
	AccessibilityScrollInfo info;
	const HRESULT result = TryGetScrollInfo(info);
	if (FAILED(result)) return result;
	*value = info.HorizontalScrollPercent;
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT ControlUiaProvider::get_HorizontalViewSize(double* value) try
{
	if (!value) return E_POINTER;
	AccessibilityScrollInfo info;
	const HRESULT result = TryGetScrollInfo(info);
	if (FAILED(result)) return result;
	*value = info.HorizontalViewSize;
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT ControlUiaProvider::get_VerticalScrollPercent(double* value) try
{
	if (!value) return E_POINTER;
	AccessibilityScrollInfo info;
	const HRESULT result = TryGetScrollInfo(info);
	if (FAILED(result)) return result;
	*value = info.VerticalScrollPercent;
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT ControlUiaProvider::get_VerticalViewSize(double* value) try
{
	if (!value) return E_POINTER;
	AccessibilityScrollInfo info;
	const HRESULT result = TryGetScrollInfo(info);
	if (FAILED(result)) return result;
	*value = info.VerticalViewSize;
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT ControlUiaProvider::get_HorizontallyScrollable(BOOL* value) try
{
	if (!value) return E_POINTER;
	AccessibilityScrollInfo info;
	const HRESULT result = TryGetScrollInfo(info);
	if (FAILED(result)) return result;
	*value = info.HorizontallyScrollable ? TRUE : FALSE;
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT ControlUiaProvider::get_VerticallyScrollable(BOOL* value) try
{
	if (!value) return E_POINTER;
	AccessibilityScrollInfo info;
	const HRESULT result = TryGetScrollInfo(info);
	if (FAILED(result)) return result;
	*value = info.VerticallyScrollable ? TRUE : FALSE;
	return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
catch (...) { return UIA_E_INVALIDOPERATION; }

HRESULT VirtualUiaProvider::CreateProviderArray(
	const std::vector<uint32_t>& ids, SAFEARRAY** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	if (!CurrentNode(nullptr) || !_root || !_owner)
		return UIA_E_ELEMENTNOTAVAILABLE;
	if (ids.size() > static_cast<size_t>(LONG_MAX))
		return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
	accessibility_detail::UniqueSafeArray result(
		::SafeArrayCreateVector(
			VT_UNKNOWN, 0, static_cast<ULONG>(ids.size())));
	if (!result) return E_OUTOFMEMORY;
	try
	{
		for (size_t position = 0; position < ids.size(); ++position)
		{
			auto* owner = _root->ResolveControl(_owner, _ownerRuntimeId);
			if (!owner) return UIA_E_ELEMENTNOTAVAILABLE;
			auto* provider = _root->VirtualProviderFor(owner, ids[position]);
			if (!provider)
				return _root->ResolveControl(_owner, _ownerRuntimeId)
					? E_OUTOFMEMORY : UIA_E_ELEMENTNOTAVAILABLE;
			LONG index = static_cast<LONG>(position);
			IUnknown* unknown =
				static_cast<IRawElementProviderSimple*>(provider);
			const HRESULT hr = ::SafeArrayPutElement(
				result.get(), &index, unknown);
			provider->Release();
			if (FAILED(hr)) return hr;
		}
	}
	catch (const std::bad_alloc&)
	{
		return E_OUTOFMEMORY;
	}
	catch (...)
	{
		return UIA_E_INVALIDOPERATION;
	}
	*value = result.release();
	return S_OK;
}

HRESULT VirtualUiaProvider::GetRowHeaderItems(SAFEARRAY** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	try
	{
		AccessibilityVirtualNode node;
		if (!CurrentNode(&node)) return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::TableItem))
			return UIA_E_NOTSUPPORTED;
		return node.RowHeaderId != 0
			? CreateProviderArray({ node.RowHeaderId }, value)
			: CreateProviderArray({}, value);
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

HRESULT VirtualUiaProvider::GetColumnHeaderItems(SAFEARRAY** value)
{
	if (!value) return E_POINTER;
	*value = nullptr;
	try
	{
		AccessibilityVirtualNode node;
		AutomationPeer* source = nullptr;
		std::shared_ptr<AutomationPeer> sourceLease;
		if (!CurrentNode(&node, &source, &sourceLease))
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::TableItem))
			return UIA_E_NOTSUPPORTED;
		std::vector<uint32_t> headers;
		source->GetAccessibilityVirtualColumnHeaders(headers);
		if (!CurrentNode(&node))
			return UIA_E_ELEMENTNOTAVAILABLE;
		if (!HasAutomationPattern(
			node.Patterns, AutomationPattern::TableItem))
			return UIA_E_NOTSUPPORTED;
		if (node.Column < 0
			|| static_cast<size_t>(node.Column) >= headers.size())
			return CreateProviderArray({}, value);
		const uint32_t header = headers[static_cast<size_t>(node.Column)];
		return header != 0
			? CreateProviderArray({ header }, value)
			: CreateProviderArray({}, value);
	}
	catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	catch (...) { return UIA_E_INVALIDOPERATION; }
}

void WindowUiaProvider::RaiseEvent(
	Control* control, AccessibilityChange change)
{
	if (!Connected() || !::UiaClientsAreListening()) return;
	if (change == AccessibilityChange::Focus
		&& !HasNativeKeyboardFocus()) return;
	if (change == AccessibilityChange::Structure)
	{
		(void)::UiaRaiseStructureChangedEvent(
			static_cast<IRawElementProviderSimple*>(this),
			StructureChangeType_ChildrenInvalidated, nullptr, 0);
		return;
	}
	if (!control) return;
	if (change == AccessibilityChange::Focus)
	{
		Control* virtualOwner = nullptr;
		uint32_t virtualId = 0;
		if (TryGetVirtualFocusedNode(
			control, virtualOwner, virtualId))
		{
			RaiseVirtualEvent(
				virtualOwner, virtualId, AccessibilityChange::Focus);
			return;
		}
	}
	auto* provider = ProviderFor(control);
	if (!provider) return;
	auto* simple = static_cast<IRawElementProviderSimple*>(provider);
	if (change == AccessibilityChange::Focus)
	{
		(void)::UiaRaiseAutomationEvent(
			simple, UIA_AutomationFocusChangedEventId);
		provider->Release();
		return;
	}
	if (change == AccessibilityChange::Scroll)
	{
		constexpr PROPERTYID properties[]{
			UIA_ScrollHorizontalScrollPercentPropertyId,
			UIA_ScrollHorizontalViewSizePropertyId,
			UIA_ScrollVerticalScrollPercentPropertyId,
			UIA_ScrollVerticalViewSizePropertyId,
			UIA_ScrollHorizontallyScrollablePropertyId,
			UIA_ScrollVerticallyScrollablePropertyId
		};
		for (const auto propertyId : properties)
		{
			VARIANT oldValue{};
			VARIANT newValue{};
			::VariantInit(&oldValue);
			::VariantInit(&newValue);
			if (SUCCEEDED(provider->GetPropertyValue(propertyId, &newValue)))
				(void)::UiaRaiseAutomationPropertyChangedEvent(
					simple, propertyId, oldValue, newValue);
			::VariantClear(&newValue);
		}
		provider->Release();
		return;
	}
	PROPERTYID propertyId = 0;
	switch (change)
	{
	case AccessibilityChange::Name: propertyId = UIA_NamePropertyId; break;
	case AccessibilityChange::Description:
		propertyId = UIA_FullDescriptionPropertyId; break;
	case AccessibilityChange::Help: propertyId = UIA_HelpTextPropertyId; break;
	case AccessibilityChange::Value:
		propertyId = provider->GetAccessibilityTypeForEvent() == 1
			? UIA_RangeValueValuePropertyId : UIA_ValueValuePropertyId;
		break;
	case AccessibilityChange::Invoke:
		(void)::UiaRaiseAutomationEvent(simple, UIA_Invoke_InvokedEventId);
		break;
	case AccessibilityChange::Toggle:
		propertyId = UIA_ToggleToggleStatePropertyId; break;
	case AccessibilityChange::ExpandCollapse:
		propertyId = UIA_ExpandCollapseExpandCollapseStatePropertyId; break;
	case AccessibilityChange::Selection:
	case AccessibilityChange::SelectionAdded:
	case AccessibilityChange::SelectionRemoved:
	case AccessibilityChange::SelectionSelected:
		propertyId = UIA_SelectionItemIsSelectedPropertyId; break;
	case AccessibilityChange::State:
		if (provider->SupportsToggleForEvent())
			propertyId = UIA_ToggleToggleStatePropertyId;
		else if (provider->SupportsSelectionItemForEvent())
			propertyId = UIA_SelectionItemIsSelectedPropertyId;
		else
			propertyId = UIA_IsEnabledPropertyId;
		break;
	case AccessibilityChange::Scroll: break;
	default: break;
	}
	if (propertyId != 0)
	{
		VARIANT oldValue{};
		VARIANT newValue{};
		::VariantInit(&oldValue);
		::VariantInit(&newValue);
		if (SUCCEEDED(provider->GetPropertyValue(propertyId, &newValue)))
			(void)::UiaRaiseAutomationPropertyChangedEvent(
				simple, propertyId, oldValue, newValue);
		::VariantClear(&newValue);
	}
	provider->Release();
}

void WindowUiaProvider::RaiseVirtualEvent(
	Control* owner, uint32_t virtualId, AccessibilityChange change)
{
	if (!Connected() || !::UiaClientsAreListening()
		|| !owner || virtualId == 0) return;
	if (change == AccessibilityChange::Focus
		&& !HasNativeKeyboardFocus()) return;
	auto* provider = VirtualProviderFor(owner, virtualId);
	if (!provider) return;
	auto* simple = static_cast<IRawElementProviderSimple*>(provider);
	if (change == AccessibilityChange::Focus)
	{
		(void)::UiaRaiseAutomationEvent(
			simple, UIA_AutomationFocusChangedEventId);
		provider->Release();
		return;
	}
	if (change == AccessibilityChange::Structure)
	{
		(void)::UiaRaiseStructureChangedEvent(simple,
			StructureChangeType_ChildrenInvalidated, nullptr, 0);
		provider->Release();
		return;
	}
	AccessibilityVirtualNode node;
	PROPERTYID propertyId = 0;
	if (ResolveVirtualNode(owner, owner->GetAccessibilityRuntimeId(),
		virtualId, node))
	{
		switch (change)
		{
		case AccessibilityChange::Name:
			propertyId = UIA_NamePropertyId; break;
		case AccessibilityChange::Description:
		case AccessibilityChange::Help:
			propertyId = UIA_FullDescriptionPropertyId; break;
		case AccessibilityChange::Value:
			propertyId = UIA_ValueValuePropertyId; break;
		case AccessibilityChange::Invoke:
			(void)::UiaRaiseAutomationEvent(simple, UIA_Invoke_InvokedEventId);
			break;
		case AccessibilityChange::Toggle:
			propertyId = UIA_ToggleToggleStatePropertyId; break;
		case AccessibilityChange::ExpandCollapse:
			propertyId = UIA_ExpandCollapseExpandCollapseStatePropertyId; break;
		case AccessibilityChange::Selection:
			propertyId = UIA_SelectionItemIsSelectedPropertyId;
			(void)::UiaRaiseAutomationEvent(simple, node.Selected
				? UIA_SelectionItem_ElementSelectedEventId
				: UIA_SelectionItem_ElementRemovedFromSelectionEventId);
			break;
		case AccessibilityChange::SelectionAdded:
			propertyId = UIA_SelectionItemIsSelectedPropertyId;
			(void)::UiaRaiseAutomationEvent(
				simple, UIA_SelectionItem_ElementAddedToSelectionEventId);
			break;
		case AccessibilityChange::SelectionRemoved:
			propertyId = UIA_SelectionItemIsSelectedPropertyId;
			(void)::UiaRaiseAutomationEvent(
				simple, UIA_SelectionItem_ElementRemovedFromSelectionEventId);
			break;
		case AccessibilityChange::SelectionSelected:
			propertyId = UIA_SelectionItemIsSelectedPropertyId;
			(void)::UiaRaiseAutomationEvent(
				simple, UIA_SelectionItem_ElementSelectedEventId);
			break;
		case AccessibilityChange::State:
			if (HasAutomationPattern(
				node.Patterns, AutomationPattern::Toggle))
				propertyId = UIA_ToggleToggleStatePropertyId;
			else if (HasAutomationPattern(
				node.Patterns, AutomationPattern::SelectionItem))
				propertyId = UIA_SelectionItemIsSelectedPropertyId;
			else
				propertyId = UIA_IsEnabledPropertyId;
			break;
		default: break;
		}
	}
	if (propertyId != 0)
	{
		VARIANT oldValue{};
		VARIANT newValue{};
		::VariantInit(&oldValue);
		::VariantInit(&newValue);
		if (SUCCEEDED(provider->GetPropertyValue(propertyId, &newValue)))
			(void)::UiaRaiseAutomationPropertyChangedEvent(
				simple, propertyId, oldValue, newValue);
		::VariantClear(&newValue);
	}
	provider->Release();
}

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace
{
	constexpr int DCompSceneLayerBand = 1000;

	D2D1_COLOR_F FromSystemColor(int index)
	{
		const COLORREF color = ::GetSysColor(index);
		return D2D1_COLOR_F{
			static_cast<float>(GetRValue(color)) / 255.0f,
			static_cast<float>(GetGValue(color)) / 255.0f,
			static_cast<float>(GetBValue(color)) / 255.0f,
			1.0f };
	}

	RECT GetPrimaryWorkArea()
	{
		RECT workArea{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
		return workArea;
	}

	RECT GetWindowWorkArea(HWND hWnd, POINT fallbackPoint)
	{
		RECT workArea = GetPrimaryWorkArea();
		HMONITOR monitor = nullptr;
		if (hWnd)
		{
			monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		}
		else
		{
			monitor = MonitorFromPoint(fallbackPoint, MONITOR_DEFAULTTONEAREST);
		}
		if (!monitor)
			return workArea;

		MONITORINFO monitorInfo{};
		monitorInfo.cbSize = sizeof(monitorInfo);
		if (GetMonitorInfoW(monitor, &monitorInfo))
			return monitorInfo.rcWork;
		return workArea;
	}

	POINT ClampWindowOriginToWorkArea(POINT origin, SIZE size, const RECT& workArea)
	{
		POINT clamped = origin;
		const int maxX = (std::max)(workArea.left, workArea.right - size.cx);
		const int maxY = (std::max)(workArea.top, workArea.bottom - size.cy);
		clamped.x = (std::clamp)(clamped.x, workArea.left, (LONG)maxX);
		clamped.y = (std::clamp)(clamped.y, workArea.top, (LONG)maxY);
		return clamped;
	}

	HICON LoadProcessIcon(bool wantSmall)
	{
		static HICON largeIcon = nullptr;
		static HICON smallIcon = nullptr;
		HICON& cached = wantSmall ? smallIcon : largeIcon;
		if (cached) return cached;

		wchar_t exePath[MAX_PATH]{};
		if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
		{
			HICON large = nullptr;
			HICON smallHandle = nullptr;
			if (ExtractIconExW(exePath, 0, &large, &smallHandle, 1) > 0)
			{
				if (wantSmall)
				{
					cached = smallHandle ? smallHandle : large;
					if (large && large != cached) DestroyIcon(large);
				}
				else
				{
					cached = large ? large : smallHandle;
					if (smallHandle && smallHandle != cached) DestroyIcon(smallHandle);
				}
			}
		}

		if (!cached)
		{
			cached = (HICON)LoadImageW(
				nullptr,
				IDI_APPLICATION,
				IMAGE_ICON,
				wantSmall ? GetSystemMetrics(SM_CXSMICON) : GetSystemMetrics(SM_CXICON),
				wantSmall ? GetSystemMetrics(SM_CYSMICON) : GetSystemMetrics(SM_CYICON),
				LR_SHARED);
		}

		return cached;
	}
}

HCURSOR Window::GetSystemCursor(CursorKind kind)
{
	static std::unordered_map<CursorKind, HCURSOR> cache;
	auto it = cache.find(kind);
	if (it != cache.end() && it->second) return it->second;

	LPCWSTR id = IDC_ARROW;
	switch (kind)
	{
	case CursorKind::Auto: id = IDC_ARROW; break;
	case CursorKind::Arrow: id = IDC_ARROW; break;
	case CursorKind::Cross: id = IDC_CROSS; break;
	case CursorKind::Hand: id = IDC_HAND; break;
	case CursorKind::IBeam: id = IDC_IBEAM; break;
	case CursorKind::SizeWE: id = IDC_SIZEWE; break;
	case CursorKind::SizeNS: id = IDC_SIZENS; break;
	case CursorKind::SizeNWSE: id = IDC_SIZENWSE; break;
	case CursorKind::SizeNESW: id = IDC_SIZENESW; break;
	case CursorKind::SizeAll: id = IDC_SIZEALL; break;
	case CursorKind::No: id = IDC_NO; break;
	default: id = IDC_ARROW; break;
	}
	HCURSOR h = LoadCursorW(nullptr, id);
	cache.emplace(kind, h);
	return h;
}

void Window::ApplyCursor(CursorKind kind)
{
	HCURSOR desired = GetSystemCursor(kind);
	if (kind == _currentCursor && ::GetCursor() == desired) return;
	_currentCursor = kind;
	::SetCursor(desired);
}

bool Window::ApplySystemCursorId(UINT32 cursorId)
{
	if (cursorId == 0) return false;
	HCURSOR cursor = LoadCursorW(nullptr, MAKEINTRESOURCEW((ULONG_PTR)cursorId));
	if (!cursor) return false;
	::SetCursor(cursor);
	return true;
}

static int ToMessageLocalCoordinate(float value)
{
	return static_cast<int>(std::floor(value));
}

static bool TryGetControlLocalPoint(
	Control* control,
	POINT contentMouse,
	int& localX,
	int& localY,
	bool requireInsideClip = true)
{
	if (!control) return false;
	D2D1_POINT_2F local{};
	if (!control->TryTransformRenderPointToLocal(
		D2D1::Point2F(
			static_cast<float>(contentMouse.x),
			static_cast<float>(contentMouse.y)), local)) return false;
	if (requireInsideClip && !control->IsRenderPointInsideClip(D2D1::Point2F(
			static_cast<float>(contentMouse.x),
			static_cast<float>(contentMouse.y)))) return false;
	localX = ToMessageLocalCoordinate(local.x);
	localY = ToMessageLocalCoordinate(local.y);
	return true;
}

static Control* HitTestDeepestChild(Control* root, POINT contentMouse)
{
	if (!root) return nullptr;
	if (!root->IsVisible || !root->IsEffectivelyEnabled()
		|| !root->ParticipatesInInputHitTesting()) return nullptr;
	int localX = 0;
	int localY = 0;
	if (!TryGetControlLocalPoint(root, contentMouse, localX, localY))
		return nullptr;
	if (!root->ShouldHitTestChildrenAt(localX, localY))
		return root;

	for (auto child : root->GetVisualChildrenInReverseZOrder())
	{
		if (!child || !child->IsVisible || !child->IsEffectivelyEnabled()
			|| !child->ParticipatesInInputHitTesting()) continue;
		int childX = 0;
		int childY = 0;
		if (TryGetControlLocalPoint(child, contentMouse, childX, childY)
			&& child->ContainsPoint(childX, childY))
		{
			auto deeperChild = HitTestDeepestChild(child, contentMouse);
			return deeperChild ? deeperChild : child;
		}
	}
	return root;
}

static bool PointInControlRect(Control* control, POINT contentMouse)
{
	if (!control) return false;
	if (!control->IsVisible || !control->IsEffectivelyEnabled()) return false;
	int localX = 0;
	int localY = 0;
	return TryGetControlLocalPoint(control, contentMouse, localX, localY)
		&& control->ContainsPoint(localX, localY);
}

static bool PointInTransientPresentationRect(
	Control* control, POINT contentMouse)
{
	if (!control) return false;
	if (!control->IsVisible || !control->IsEffectivelyEnabled()) return false;
	int localX = 0;
	int localY = 0;
	return TryGetControlLocalPoint(control, contentMouse, localX, localY)
		&& control->ContainsPoint(localX, localY);
}

static bool IsControlOrDescendantOf(Control* control, Control* ancestor)
{
	for (auto current = control; current; current = current->GetVisualParent())
	{
		if (current == ancestor)
			return true;
	}
	return false;
}

static void SyncNativeWindowStyles(
	HWND hWnd,
	bool showInTaskbar,
	WindowStyle windowStyle,
	ResizeMode resizeMode)
{
	if (!hWnd)
		return;
	const bool hasChrome = windowStyle != WindowStyle::None;
	const bool toolWindow = windowStyle == WindowStyle::ToolWindow;
	const bool canMinimize = hasChrome && !toolWindow
		&& resizeMode != ResizeMode::NoResize;
	const bool canMaximize = hasChrome && !toolWindow
		&& (resizeMode == ResizeMode::CanResize
			|| resizeMode == ResizeMode::CanResizeWithGrip);
	const bool canResize = resizeMode == ResizeMode::CanResize
		|| resizeMode == ResizeMode::CanResizeWithGrip;

	LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
	style &= ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_THICKFRAME);
	if (canMinimize) style |= WS_MINIMIZEBOX;
	if (canMaximize) style |= WS_MAXIMIZEBOX;
	if (hasChrome) style |= WS_SYSMENU;
	if (canResize) style |= WS_THICKFRAME;
	SetWindowLongPtrW(hWnd, GWL_STYLE, style);

	LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
	if (showInTaskbar && !toolWindow)
	{
		exStyle &= ~WS_EX_TOOLWINDOW;
		exStyle |= WS_EX_APPWINDOW;
	}
	else
	{
		exStyle &= ~WS_EX_APPWINDOW;
		exStyle |= WS_EX_TOOLWINDOW;
	}
	SetWindowLongPtrW(hWnd, GWL_EXSTYLE, exStyle);

	SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

	const int cornerPreference = 1;
	DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
}

static int GetCustomFrameInset()
{
	return GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}

static std::vector<Control*> GetRootControlsInZOrder(Window* form)
{
	std::vector<Control*> result;
	if (!form) return result;
	result.assign(form->GetVisualChildrenView().begin(), form->GetVisualChildrenView().end());
	std::stable_sort(result.begin(), result.end(), [](Control* left, Control* right)
		{
			if (!left || !right) return left != nullptr;
			return left->ZIndex < right->ZIndex;
		});
	return result;
}

static std::vector<Control*> GetRootControlsInReverseZOrder(Window* form)
{
	auto result = GetRootControlsInZOrder(form);
	std::reverse(result.begin(), result.end());
	return result;
}

Control* Window::HitTestControlAt(POINT contentMouse)
{
	// Transient presentation roots are hit-tested from topmost to bottommost.
	auto transientRoots = GetTransientPresentationRoots();
	for (auto it = transientRoots.rbegin(); it != transientRoots.rend(); ++it)
	{
		auto* transientRoot = *it;
		if (PointInTransientPresentationRect(transientRoot, contentMouse))
			return HitTestDeepestChild(transientRoot, contentMouse);
	}

	// Content tree: hit test in reverse render order.
	for (auto control : GetRootControlsInReverseZOrder(this))
	{
		if (!control || !control->IsVisible
			|| !control->IsEffectivelyEnabled()) continue;
		if (IsTransientPresentationOpen(control)) continue;
		if (!PointInControlRect(control, contentMouse)) continue;
		return HitTestDeepestChild(control, contentMouse);
	}
	return nullptr;
}

static bool IsAncestorNavigationFallbackKey(Key key)
{
	switch (key)
	{
	case Key::Tab:
	case Key::Left:
	case Key::Right:
	case Key::Up:
	case Key::Down:
	case Key::Home:
	case Key::End:
	case Key::PageUp:
	case Key::PageDown:
	case Key::Return:
	case Key::Escape:
		return true;
	default:
		return false;
	}
}

static Control* FindAncestorNavigationFallback(Control* start, Key key)
{
	if (!start) return nullptr;
	for (Control* parent = start->GetVisualParent(); parent;
		parent = parent->GetVisualParent())
	{
		if (parent->HandlesNavigationKey(key))
			return parent;
	}
	return nullptr;
}

static Control* GetAncestorNavigationFallbackTarget(Control* selected, Key key)
{
	if (!selected) return nullptr;
	if (!selected->IsVisible) return nullptr;
	if (!IsAncestorNavigationFallbackKey(key)) return nullptr;
	if (selected->HandlesNavigationKey(key)) return nullptr;
	return FindAncestorNavigationFallback(selected, key);
}

static bool DataObjectHasFormat(IDataObject* pDataObj, CLIPFORMAT cf)
{
	if (!pDataObj) return false;
	FORMATETC fmt{};
	fmt.cfFormat = cf;
	fmt.dwAspect = DVASPECT_CONTENT;
	fmt.lindex = -1;
	fmt.tymed = TYMED_HGLOBAL;
	return SUCCEEDED(pDataObj->QueryGetData(&fmt));
}

static std::optional<std::vector<std::wstring>> TryExtractDroppedFiles(IDataObject* pDataObj)
{
	if (!pDataObj) return std::nullopt;
	FORMATETC fmt{};
	fmt.cfFormat = CF_HDROP;
	fmt.dwAspect = DVASPECT_CONTENT;
	fmt.lindex = -1;
	fmt.tymed = TYMED_HGLOBAL;
	STGMEDIUM stg{};
	if (FAILED(pDataObj->GetData(&fmt, &stg))) return std::nullopt;

	std::vector<std::wstring> files;
	HDROP hDrop = (HDROP)GlobalLock(stg.hGlobal);
	if (hDrop)
	{
		UINT count = DragQueryFile(hDrop, 0xFFFFFFFF, nullptr, 0);
		for (UINT i = 0; i < count; i++)
		{
			const UINT length = DragQueryFileW(hDrop, i, nullptr, 0);
			std::wstring path(length, L'\0');
			if (length != 0)
			{
				path.resize(static_cast<size_t>(length) + 1);
				(void)DragQueryFileW(hDrop, i, path.data(), length + 1);
				path.resize(length);
				files.push_back(std::move(path));
			}
		}
		GlobalUnlock(stg.hGlobal);
	}
	ReleaseStgMedium(&stg);
	if (files.size() <= 0) return std::nullopt;
	return files;
}

static std::optional<std::wstring> TryExtractDroppedText(IDataObject* pDataObj)
{
	if (!pDataObj) return std::nullopt;
	CLIPFORMAT fmtText = CF_UNICODETEXT;
	if (!DataObjectHasFormat(pDataObj, fmtText))
	{
		fmtText = CF_TEXT;
		if (!DataObjectHasFormat(pDataObj, fmtText))
			return std::nullopt;
	}

	FORMATETC fmt{};
	fmt.cfFormat = fmtText;
	fmt.dwAspect = DVASPECT_CONTENT;
	fmt.lindex = -1;
	fmt.tymed = TYMED_HGLOBAL;
	STGMEDIUM stg{};
	if (FAILED(pDataObj->GetData(&fmt, &stg))) return std::nullopt;

	std::optional<std::wstring> result;
	void* p = GlobalLock(stg.hGlobal);
	if (p)
	{
		if (fmtText == CF_UNICODETEXT)
		{
			result = std::wstring((const wchar_t*)p);
		}
		else
		{
			// ANSI -> UTF-16
			const char* s = (const char*)p;
			int len = (int)strlen(s);
			int wlen = MultiByteToWideChar(CP_ACP, 0, s, len, nullptr, 0);
			std::wstring ws;
			ws.resize(wlen);
			if (wlen > 0)
				MultiByteToWideChar(CP_ACP, 0, s, len, ws.data(), wlen);
			result = std::move(ws);
		}
		GlobalUnlock(stg.hGlobal);
	}
	ReleaseStgMedium(&stg);
	if (result && result->empty()) return std::nullopt;
	return result;
}

static DragDropEffects FromOleDropEffects(DWORD effects) noexcept
{
	std::uint32_t value = 0;
	if ((effects & DROPEFFECT_COPY) != 0)
		value |= static_cast<std::uint32_t>(DragDropEffects::Copy);
	if ((effects & DROPEFFECT_MOVE) != 0)
		value |= static_cast<std::uint32_t>(DragDropEffects::Move);
	if ((effects & DROPEFFECT_LINK) != 0)
		value |= static_cast<std::uint32_t>(DragDropEffects::Link);
	if ((effects & DROPEFFECT_SCROLL) != 0)
		value |= static_cast<std::uint32_t>(DragDropEffects::Scroll);
	return static_cast<DragDropEffects>(value);
}

static DWORD ToOleDropEffects(DragDropEffects effects) noexcept
{
	DWORD value = DROPEFFECT_NONE;
	if ((effects & DragDropEffects::Copy) != DragDropEffects::None)
		value |= DROPEFFECT_COPY;
	if ((effects & DragDropEffects::Move) != DragDropEffects::None)
		value |= DROPEFFECT_MOVE;
	if ((effects & DragDropEffects::Link) != DragDropEffects::None)
		value |= DROPEFFECT_LINK;
	if ((effects & DragDropEffects::Scroll) != DragDropEffects::None)
		value |= DROPEFFECT_SCROLL;
	return value;
}

static DragDropKeyStates FromOleDropKeyState(DWORD state) noexcept
{
	return static_cast<DragDropKeyStates>(state &
		(MK_LBUTTON | MK_RBUTTON | MK_SHIFT | MK_CONTROL | MK_MBUTTON | 0x20));
}

static std::shared_ptr<const DragDataObject> ProjectDragData(IDataObject* data)
{
	std::vector<std::wstring> files;
	if (auto value = TryExtractDroppedFiles(data)) files = std::move(*value);
	auto text = TryExtractDroppedText(data);
	return std::make_shared<const DragDataObject>(
		std::move(files), std::move(text));
}

class WindowDropTarget final : public IDropTarget
{
public:
	explicit WindowDropTarget(Window* window) : _ref(1), _window(window) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(
		REFIID riid, void** object) override
	{
		if (!object) return E_POINTER;
		*object = nullptr;
		if (riid == IID_IUnknown || riid == IID_IDropTarget)
		{
			*object = static_cast<IDropTarget*>(this);
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef(void) override
	{
		return InterlockedIncrement(&_ref);
	}

	ULONG STDMETHODCALLTYPE Release(void) override
	{
		const ULONG result = InterlockedDecrement(&_ref);
		if (result == 0) delete this;
		return result;
	}

	HRESULT STDMETHODCALLTYPE DragEnter(
		IDataObject* data,
		DWORD keyState,
		POINTL screenPoint,
		DWORD* effect) override
	{
		if (!effect) return E_POINTER;
		_allowedEffects = FromOleDropEffects(*effect);
		_data = ProjectDragData(data);
		UpdatePointer(keyState, screenPoint);
		auto* target = ResolveDropTarget();
		_currentTarget = target;
		*effect = ToOleDropEffects(Raise(
			target, RoutedEventId::DragEnter, keyState));
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DragOver(
		DWORD keyState,
		POINTL screenPoint,
		DWORD* effect) override
	{
		if (!effect) return E_POINTER;
		UpdatePointer(keyState, screenPoint);
		auto* previous = _currentTarget.Get();
		auto* current = ResolveDropTarget();
		DragDropEffects result = DragDropEffects::None;
		if (previous != current)
		{
			(void)Raise(previous, RoutedEventId::DragLeave, keyState);
			_currentTarget = current;
			result = Raise(current, RoutedEventId::DragEnter, keyState);
		}
		else
		{
			result = Raise(current, RoutedEventId::DragOver, keyState);
		}
		*effect = ToOleDropEffects(result);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DragLeave(void) override
	{
		(void)Raise(_currentTarget.Get(), RoutedEventId::DragLeave, _keyState);
		ResetTransaction();
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Drop(
		IDataObject* data,
		DWORD keyState,
		POINTL screenPoint,
		DWORD* effect) override
	{
		if (!effect) return E_POINTER;
		const auto suppliedEffects = FromOleDropEffects(*effect);
		if (suppliedEffects != DragDropEffects::None)
			_allowedEffects = suppliedEffects;
		_data = ProjectDragData(data);
		UpdatePointer(keyState, screenPoint);
		auto* previous = _currentTarget.Get();
		auto* current = ResolveDropTarget();
		if (previous != current)
		{
			(void)Raise(previous, RoutedEventId::DragLeave, keyState);
			_currentTarget = current;
			(void)Raise(current, RoutedEventId::DragEnter, keyState);
		}
		*effect = ToOleDropEffects(
			Raise(current, RoutedEventId::Drop, keyState));
		ResetTransaction();
		return S_OK;
	}

private:
	bool HasSupportedData() const noexcept
	{
		return _data && (_data->HasFiles() || _data->HasText());
	}

	void UpdatePointer(DWORD keyState, POINTL screenPoint)
	{
		_keyState = keyState;
		_hasContentPoint = false;
		if (!_window || !_window->Handle) return;
		POINT client{ screenPoint.x, screenPoint.y };
		if (!::ScreenToClient(_window->Handle, &client)) return;
		const int titlePixels = _window->GetTitleBarHeightPixels();
		if (_window->HasWindowChrome() && client.y < titlePixels) return;
		const float scale = _window->GetDpiScale();
		_rootX = static_cast<float>(client.x) / scale;
		_rootY = static_cast<float>(client.y - titlePixels) / scale;
		_hasContentPoint = true;
	}

	Control* ResolveDropTarget() const
	{
		if (!_window || !_hasContentPoint || !HasSupportedData()) return nullptr;
		POINT point{
			static_cast<LONG>(std::floor(_rootX)),
			static_cast<LONG>(std::floor(_rootY)) };
		Control* hit = _window->HitTestControlAt(point);
		if (!hit) hit = _window;
		for (auto* current = hit; current; current = current->GetRoutedParent())
			if (current->AllowDrop && current->IsEffectivelyEnabled()
				&& current->GetIsVisible()) return current;
		return nullptr;
	}

	DragDropEffects Raise(
		Control* target,
		RoutedEventId eventId,
		DWORD keyState) const
	{
		if (!target || !_data) return DragDropEffects::None;
		DragEventArgs args(
			_data, FromOleDropKeyState(keyState), _allowedEffects,
			_rootX, _rootY);
		(void)RaiseRoutedEvent(*target, eventId, args);
		return args.Effects & args.AllowedEffects;
	}

	void ResetTransaction() noexcept
	{
		_currentTarget.Reset();
		_data.reset();
		_allowedEffects = DragDropEffects::None;
		_keyState = 0;
		_hasContentPoint = false;
	}

	volatile LONG _ref;
	Window* _window = nullptr;
	ControlWeakReference _currentTarget;
	std::shared_ptr<const DragDataObject> _data;
	DragDropEffects _allowedEffects = DragDropEffects::None;
	DWORD _keyState = 0;
	float _rootX = 0.0f;
	float _rootY = 0.0f;
	bool _hasContentPoint = false;
};

static ModifierKeys CurrentNativeModifierKeys() noexcept
{
	auto modifiers = ModifierKeys::None;
	if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0)
		modifiers |= ModifierKeys::Control;
	if ((::GetKeyState(VK_MENU) & 0x8000) != 0)
		modifiers |= ModifierKeys::Alt;
	if ((::GetKeyState(VK_SHIFT) & 0x8000) != 0)
		modifiers |= ModifierKeys::Shift;
	if ((::GetKeyState(VK_LWIN) & 0x8000) != 0
		|| (::GetKeyState(VK_RWIN) & 0x8000) != 0)
		modifiers |= ModifierKeys::Windows;
	return modifiers;
}

static MouseButton NativeChangedMouseButton(
	UINT message, WPARAM wParam) noexcept
{
	switch (message)
	{
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
		return MouseButton::Left;
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
		return MouseButton::Right;
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
		return MouseButton::Middle;
	case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
		return GET_XBUTTON_WPARAM(wParam) == XBUTTON1
			? MouseButton::XButton1 : MouseButton::XButton2;
	default:
		return MouseButton::None;
	}
}

static MouseButtonStates NativeMouseButtonStates(WPARAM wParam) noexcept
{
	const UINT state = GET_KEYSTATE_WPARAM(wParam);
	MouseButtonStates buttons;
	if ((state & MK_LBUTTON) != 0)
		buttons.LeftButton = MouseButtonState::Pressed;
	if ((state & MK_RBUTTON) != 0)
		buttons.RightButton = MouseButtonState::Pressed;
	if ((state & MK_MBUTTON) != 0)
		buttons.MiddleButton = MouseButtonState::Pressed;
	if ((state & MK_XBUTTON1) != 0)
		buttons.XButton1 = MouseButtonState::Pressed;
	if ((state & MK_XBUTTON2) != 0)
		buttons.XButton2 = MouseButtonState::Pressed;
	return buttons;
}

static ModifierKeys NativePointerModifierKeys(WPARAM wParam) noexcept
{
	const UINT state = GET_KEYSTATE_WPARAM(wParam);
	auto modifiers = CurrentNativeModifierKeys()
		& (ModifierKeys::Alt | ModifierKeys::Windows);
	if ((state & MK_CONTROL) != 0)
		modifiers |= ModifierKeys::Control;
	if ((state & MK_SHIFT) != 0)
		modifiers |= ModifierKeys::Shift;
	return modifiers;
}

/** The only Win32 virtual-key -> WPF Key conversion boundary. */
static Key NativeKeyFromVirtualKey(UINT virtualKey) noexcept
{
	auto offset = [](Key first, UINT value, UINT nativeFirst) noexcept
	{
		return static_cast<Key>(static_cast<std::uint16_t>(first)
			+ static_cast<std::uint16_t>(value - nativeFirst));
	};
	if (virtualKey >= '0' && virtualKey <= '9')
		return offset(Key::D0, virtualKey, '0');
	if (virtualKey >= 'A' && virtualKey <= 'Z')
		return offset(Key::A, virtualKey, 'A');
	if (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9)
		return offset(Key::NumPad0, virtualKey, VK_NUMPAD0);
	if (virtualKey >= VK_F1 && virtualKey <= VK_F24)
		return offset(Key::F1, virtualKey, VK_F1);

	switch (virtualKey)
	{
	case VK_CANCEL: return Key::Cancel;
	case VK_BACK: return Key::Back;
	case VK_TAB: return Key::Tab;
	case VK_CLEAR: return Key::Clear;
	case VK_RETURN: return Key::Return;
	case VK_PAUSE: return Key::Pause;
	case VK_CAPITAL: return Key::Capital;
	case VK_KANA: return Key::KanaMode;
	case VK_JUNJA: return Key::JunjaMode;
	case VK_FINAL: return Key::FinalMode;
	case VK_KANJI: return Key::KanjiMode;
	case VK_ESCAPE: return Key::Escape;
	case VK_CONVERT: return Key::ImeConvert;
	case VK_NONCONVERT: return Key::ImeNonConvert;
	case VK_ACCEPT: return Key::ImeAccept;
	case VK_MODECHANGE: return Key::ImeModeChange;
	case VK_SPACE: return Key::Space;
	case VK_PRIOR: return Key::Prior;
	case VK_NEXT: return Key::Next;
	case VK_END: return Key::End;
	case VK_HOME: return Key::Home;
	case VK_LEFT: return Key::Left;
	case VK_UP: return Key::Up;
	case VK_RIGHT: return Key::Right;
	case VK_DOWN: return Key::Down;
	case VK_SELECT: return Key::Select;
	case VK_PRINT: return Key::Print;
	case VK_EXECUTE: return Key::Execute;
	case VK_SNAPSHOT: return Key::Snapshot;
	case VK_INSERT: return Key::Insert;
	case VK_DELETE: return Key::Delete;
	case VK_HELP: return Key::Help;
	case VK_LWIN: return Key::LWin;
	case VK_RWIN: return Key::RWin;
	case VK_APPS: return Key::Apps;
	case VK_SLEEP: return Key::Sleep;
	case VK_MULTIPLY: return Key::Multiply;
	case VK_ADD: return Key::Add;
	case VK_SEPARATOR: return Key::Separator;
	case VK_SUBTRACT: return Key::Subtract;
	case VK_DECIMAL: return Key::Decimal;
	case VK_DIVIDE: return Key::Divide;
	case VK_NUMLOCK: return Key::NumLock;
	case VK_SCROLL: return Key::Scroll;
	case VK_SHIFT:
	case VK_LSHIFT: return Key::LeftShift;
	case VK_RSHIFT: return Key::RightShift;
	case VK_CONTROL:
	case VK_LCONTROL: return Key::LeftCtrl;
	case VK_RCONTROL: return Key::RightCtrl;
	case VK_MENU:
	case VK_LMENU: return Key::LeftAlt;
	case VK_RMENU: return Key::RightAlt;
	case VK_BROWSER_BACK: return Key::BrowserBack;
	case VK_BROWSER_FORWARD: return Key::BrowserForward;
	case VK_BROWSER_REFRESH: return Key::BrowserRefresh;
	case VK_BROWSER_STOP: return Key::BrowserStop;
	case VK_BROWSER_SEARCH: return Key::BrowserSearch;
	case VK_BROWSER_FAVORITES: return Key::BrowserFavorites;
	case VK_BROWSER_HOME: return Key::BrowserHome;
	case VK_VOLUME_MUTE: return Key::VolumeMute;
	case VK_VOLUME_DOWN: return Key::VolumeDown;
	case VK_VOLUME_UP: return Key::VolumeUp;
	case VK_MEDIA_NEXT_TRACK: return Key::MediaNextTrack;
	case VK_MEDIA_PREV_TRACK: return Key::MediaPreviousTrack;
	case VK_MEDIA_STOP: return Key::MediaStop;
	case VK_MEDIA_PLAY_PAUSE: return Key::MediaPlayPause;
	case VK_LAUNCH_MAIL: return Key::LaunchMail;
	case VK_LAUNCH_MEDIA_SELECT: return Key::SelectMedia;
	case VK_LAUNCH_APP1: return Key::LaunchApplication1;
	case VK_LAUNCH_APP2: return Key::LaunchApplication2;
	case VK_OEM_1: return Key::Oem1;
	case VK_OEM_PLUS: return Key::OemPlus;
	case VK_OEM_COMMA: return Key::OemComma;
	case VK_OEM_MINUS: return Key::OemMinus;
	case VK_OEM_PERIOD: return Key::OemPeriod;
	case VK_OEM_2: return Key::Oem2;
	case VK_OEM_3: return Key::Oem3;
	case 0xC1: return Key::AbntC1;
	case 0xC2: return Key::AbntC2;
	case VK_OEM_4: return Key::Oem4;
	case VK_OEM_5: return Key::Oem5;
	case VK_OEM_6: return Key::Oem6;
	case VK_OEM_7: return Key::Oem7;
	case VK_OEM_8: return Key::Oem8;
	case VK_OEM_102: return Key::Oem102;
	case VK_PROCESSKEY: return Key::ImeProcessed;
	case 0xF0: return Key::OemAttn;
	case 0xF1: return Key::OemFinish;
	case 0xF2: return Key::OemCopy;
	case 0xF3: return Key::OemAuto;
	case 0xF4: return Key::OemEnlw;
	case 0xF5: return Key::OemBackTab;
	case VK_ATTN: return Key::Attn;
	case VK_CRSEL: return Key::CrSel;
	case VK_EXSEL: return Key::ExSel;
	case VK_EREOF: return Key::EraseEof;
	case VK_PLAY: return Key::Play;
	case VK_ZOOM: return Key::Zoom;
	case VK_NONAME: return Key::NoName;
	case VK_PA1: return Key::Pa1;
	case VK_OEM_CLEAR: return Key::OemClear;
	default: return Key::None;
	}
}

static std::optional<InputReport> CreateNativeInputReport(
	UINT message, WPARAM wParam, LPARAM lParam, POINT contentPoint)
{
	InputReport input;
	input.X = contentPoint.x;
	input.Y = contentPoint.y;
	input.Modifiers = CurrentNativeModifierKeys();
	input.ChangedButton = NativeChangedMouseButton(message, wParam);
	switch (message)
	{
	case WM_MOUSEMOVE:
		input.Kind = InputReportKind::PointerMove;
		input.Modifiers = NativePointerModifierKeys(wParam);
		input.ButtonStates = NativeMouseButtonStates(wParam);
		break;
	case WM_MOUSELEAVE:
		input.Kind = InputReportKind::PointerLeave; break;
	case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN:
		input.Kind = InputReportKind::PointerDown;
		input.Modifiers = NativePointerModifierKeys(wParam);
		input.ButtonStates = NativeMouseButtonStates(wParam);
		input.ClickCount = 1;
		break;
	case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
	case WM_XBUTTONUP:
		input.Kind = InputReportKind::PointerUp;
		input.Modifiers = NativePointerModifierKeys(wParam);
		input.ButtonStates = NativeMouseButtonStates(wParam);
		break;
	case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
	case WM_XBUTTONDBLCLK:
		input.Kind = InputReportKind::PointerDoubleClick;
		input.Modifiers = NativePointerModifierKeys(wParam);
		input.ButtonStates = NativeMouseButtonStates(wParam);
		input.ClickCount = 2;
		break;
	case WM_MOUSEWHEEL:
		input.Kind = InputReportKind::MouseWheel;
		input.Modifiers = NativePointerModifierKeys(wParam);
		input.ButtonStates = NativeMouseButtonStates(wParam);
		input.WheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
		break;
	case WM_MOUSEHWHEEL:
		input.Kind = InputReportKind::HorizontalMouseWheel;
		input.Modifiers = NativePointerModifierKeys(wParam);
		input.ButtonStates = NativeMouseButtonStates(wParam);
		input.WheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
		break;
	case WM_KEYDOWN: case WM_SYSKEYDOWN:
		input.Kind = InputReportKind::KeyDown;
		input.Key = NativeKeyFromVirtualKey(static_cast<UINT>(wParam));
		if (message == WM_SYSKEYDOWN)
			input.SystemKey = input.Key;
		input.IsRepeat = (static_cast<unsigned long long>(lParam)
			& (1ull << 30)) != 0;
		break;
	case WM_KEYUP: case WM_SYSKEYUP:
		input.Kind = InputReportKind::KeyUp;
		input.Key = NativeKeyFromVirtualKey(static_cast<UINT>(wParam));
		if (message == WM_SYSKEYUP)
			input.SystemKey = input.Key;
		break;
	case WM_SETFOCUS:
		input.Kind = InputReportKind::FocusGained; break;
	case WM_KILLFOCUS:
		input.Kind = InputReportKind::FocusLost; break;
	case WM_CAPTURECHANGED:
		input.Kind = InputReportKind::CaptureLost; break;
	case WM_CANCELMODE:
		input.Kind = InputReportKind::Cancel; break;
	default:
		return std::nullopt;
	}
	return input;
}

CursorKind Window::QueryCursorAt(POINT mouseClient, POINT contentMouse)
{
	const int titleBarHeight = GetTitleBarHeightPixels();
	if (this->HasWindowChrome() && mouseClient.y < titleBarHeight)
	{
		return CursorKind::Arrow;
	}

	if (auto* captured = GetMouseCaptured(); captured && captured->IsVisible)
	{
		int localX = 0;
		int localY = 0;
		if (TryGetControlLocalPoint(
			captured, contentMouse, localX, localY, false))
			return captured->ResolvePointerCursor(localX, localY);
	}

	auto hitControl = HitTestControlAt(contentMouse);

	if (!hitControl) return CursorKind::Arrow;
	// Cursor is inherited, but an Auto descendant's native behavior hook is not.
	// Walk the routed ancestry just like pointer input so decorative template
	// children do not hide a region-sensitive host cursor (for example a
	// DataGridColumnHeader gripper). A concrete authored/inherited Cursor stays
	// authoritative, while Arrow from an Auto behavior allows the parent to
	// contribute a more specific shape.
	for (auto* target = hitControl; target; target = target->GetRoutedParent())
	{
		int localX = 0;
		int localY = 0;
		if (!TryGetControlLocalPoint(
			target, contentMouse, localX, localY, false)) continue;
		if (target->Cursor != CursorKind::Auto)
			return target->Cursor;
		const auto behavior = target->ResolvePointerCursor(localX, localY);
		if (behavior != CursorKind::Arrow) return behavior;
	}
	return CursorKind::Arrow;
}

void Window::UpdateCursor(POINT mouseClient, POINT contentMouse)
{
	const int titleBarHeight = GetTitleBarHeightPixels();
	if (!(this->HasWindowChrome() && mouseClient.y < titleBarHeight))
	{
		if (auto* captured = GetMouseCaptured(); captured && captured->IsVisible)
		{
			UINT32 cursorId = 0;
			if (captured->TryGetSystemCursorId(cursorId)
				&& ApplySystemCursorId(cursorId)) return;
			// While native capture is active the pointer routes directly to this
			// control even outside its clip.  Resolve its behavioral cursor without
			// walking the visual tree; large virtualized surfaces otherwise pay two
			// full hit tests for every drag move.
			ApplyCursor(QueryCursorAt(mouseClient, contentMouse));
			return;
		}

		auto hitControl = HitTestControlAt(contentMouse);
		for (Control* target = hitControl; target;
			target = target->GetRoutedParent())
		{
			UINT32 cursorId = 0;
			if (target->TryGetSystemCursorId(cursorId) && ApplySystemCursorId(cursorId))
				return;
		}
	}

	ApplyCursor(QueryCursorAt(mouseClient, contentMouse));
}

void Window::UpdateCursorFromCurrentMouse()
{
	if (!this->Handle) return;
	POINT mouse{};
	GetCursorPos(&mouse);
	ScreenToClient(this->Handle, &mouse);
	const float dpiScale = GetDpiScale();
	POINT contentMouse{ (LONG)(mouse.x / dpiScale), (LONG)((mouse.y - GetTitleBarHeightPixels()) / dpiScale) };
	Control* directlyOver = nullptr;
	if (!(HasWindowChrome() && mouse.y < GetTitleBarHeightPixels()))
		directlyOver = HitTestControlAt(contentMouse);
	UpdateMouseOverProjection(directlyOver, contentMouse);
	UpdateCursor(mouse, contentMouse);
}

void Window::PublishKeyboardFocusTransition(
	Control* previousSelection,
	Control* value,
	bool invalidateVisual)
{
	if (_uiaProvider) _uiaProvider->ClearVirtualFocus();
	if (previousSelection)
	{
		if (invalidateVisual) previousSelection->InvalidateVisual();
		NotifyAccessibilityEvent(previousSelection, AccessibilityChange::State);
	}
	if (value)
	{
		if (invalidateVisual) value->InvalidateVisual();
		NotifyAccessibilityEvent(value, AccessibilityChange::Focus);
	}
	RefreshKeyboardFocusVisual();
	RefreshDefaultedButtons();
}

void Window::RecordMostRecentInputDevice(const InputReport& input)
{
	bool keyboard = false;
	switch (input.Kind)
	{
	case InputReportKind::KeyDown:
	case InputReportKind::KeyUp:
		keyboard = true;
		break;
	case InputReportKind::PointerMove:
	case InputReportKind::PointerLeave:
	case InputReportKind::PointerDown:
	case InputReportKind::PointerUp:
	case InputReportKind::PointerDoubleClick:
	case InputReportKind::MouseWheel:
	case InputReportKind::HorizontalMouseWheel:
		break;
	default:
		return;
	}
	if (_keyboardMostRecentInputDevice == keyboard) return;
	_keyboardMostRecentInputDevice = keyboard;
	RefreshKeyboardFocusVisual();
}

void Window::RefreshKeyboardFocusVisual()
{
	auto* focused = GetKeyboardFocusedElement();
	if (!focused) return;
	focused->SetIsKeyboardFocusVisibleCore(
		_systemVisualPreferences.KeyboardCuesAlwaysVisible
		|| _keyboardMostRecentInputDevice);
}

void Window::PublishLogicalFocusTransition(
	Control* previous,
	Control* current)
{
	if (_inputManager)
		_inputManager->NotifyLogicalFocusChanged(previous, current);
}

void Window::SetKeyboardFocus(
	Control* value,
	bool invalidateVisual,
	FocusChangeReason reason)
{
	if (_focusManager)
		(void)_focusManager->SetKeyboardFocus(value, invalidateVisual, reason);
}

Control* Window::GetFocusScope(Control* element) const noexcept
{
	return _focusManager ? _focusManager->GetFocusScope(element) : nullptr;
}

Control* Window::GetLogicalFocusedElement(Control* scope) const noexcept
{
	return _focusManager
		? _focusManager->GetLogicalFocusedElement(scope) : nullptr;
}

bool Window::SetLogicalFocus(
	Control* scope,
	Control* element,
	bool moveKeyboardFocus,
	bool invalidateVisual)
{
	return _focusManager && _focusManager->SetLogicalFocus(
		scope, element, moveKeyboardFocus, invalidateVisual);
}

bool Window::CaptureMouse(Control* value)
{
	return _inputManager && _inputManager->CaptureMouse(*this, value);
}

bool Window::ReleaseMouseCapture(Control* expectedOwner)
{
	return _inputManager
		&& _inputManager->ReleaseMouseCapture(*this, expectedOwner);
}

Control* Window::GetMouseCaptured() const noexcept
{
	return _inputManager ? _inputManager->MouseCaptured() : nullptr;
}

std::vector<Control*> Window::BuildTabOrder(std::span<Control* const> roots)
{
	return FocusManager::BuildTabOrder(roots);
}

std::vector<Control*> Window::GetTabOrder() const
{
	return _focusManager ? _focusManager->GetTabOrder() : std::vector<Control*>{};
}

std::vector<Control*> Window::GetAccessibleControls() const
{
	const ControlWeakReference windowLifetime(
		const_cast<Window*>(this));
	// Build the logical-tree snapshot before asking any control to lazily
	// create its AutomationPeer. Peer creation is an extensibility boundary
	// and may synchronously remove controls that were visited earlier; keeping
	// only raw pointers across that callback would make the returned list
	// contain dangling entries.
	struct Candidate
	{
		ControlWeakReference Lifetime;
		size_t Depth = 0;
	};
	std::vector<Candidate> candidates;
	auto collect = [&](Control* control, size_t depth,
		const auto& self) -> void
	{
		if (!control) return;
		candidates.push_back({ ControlWeakReference(control), depth });
		for (auto* child : control->GetLogicalChildrenView())
			self(child, depth + 1, self);
	};
	for (auto* root : GetLogicalChildrenView())
		collect(root, 0, collect);

	std::vector<ControlWeakReference> accessible;
	accessible.reserve(candidates.size());
	constexpr size_t noSuppressedSubtree = (std::numeric_limits<size_t>::max)();
	size_t suppressedParentDepth = noSuppressedSubtree;
	for (const auto& candidate : candidates)
	{
		if (suppressedParentDepth != noSuppressedSubtree)
		{
			if (candidate.Depth > suppressedParentDepth) continue;
			suppressedParentDepth = noSuppressedSubtree;
		}
		auto* control = candidate.Lifetime.Get();
		if (!control) continue;
		accessible.emplace_back(control);
		const ControlWeakReference controlLifetime(control);
		const bool virtualized = control->GetAutomationPeer()
			.SupportsVirtualizedChildren();
		if (!windowLifetime.Get()) return {};
		if (!controlLifetime.Get())
		{
			accessible.pop_back();
			continue;
		}
		if (virtualized) suppressedParentDepth = candidate.Depth;
	}

	std::vector<Control*> result;
	if (!windowLifetime.Get()) return result;
	result.reserve(accessible.size());
	for (const auto& reference : accessible)
	{
		auto* control = reference.Get();
		if (!control) continue;
		// A peer callback may also have detached or reparented a still-live
		// control. Only publish controls that remain in this Window's logical
		// tree after all callbacks have completed.
		auto* ancestor = control;
		while (ancestor && ancestor != this)
			ancestor = ancestor->GetLogicalParent();
		if (ancestor == this) result.push_back(control);
	}
	return result;
}

Control* Window::ResolveAccessibleControl(Control* candidate) const noexcept
{
	try
	{
		if (!candidate) return nullptr;

		// Provider property queries are extremely dense: UIAutomationCore commonly
		// asks a focused editor for dozens of properties in one pass. Rebuilding
		// the entire accessible tree for every query turns a DataGrid edit into
		// O(tree size * property count) work and can starve the HWND input queue.
		// Membership in the logical accessibility projection can be decided by the
		// candidate's ancestor chain instead. A virtualized owner is included, but
		// its realized descendants are suppressed exactly as GetAccessibleControls
		// suppresses their depth range.
		const ControlWeakReference windowLifetime(
			const_cast<Window*>(this));
		const ControlWeakReference candidateLifetime(candidate);
		const uint32_t candidateRuntimeId =
			candidate->GetAccessibilityRuntimeId();
		std::unordered_set<Control*> visited;
		Control* current = candidate;
		while (current && current != this)
		{
			if (!visited.insert(current).second) return nullptr;
			const ControlWeakReference currentLifetime(current);
			auto* parent = current->GetLogicalParent();
			if (!parent) return nullptr;
			const ControlWeakReference parentLifetime(parent);

			if (parent != this)
			{
				auto peer = parent->AcquireAutomationPeer();
				auto* window = dynamic_cast<Window*>(windowLifetime.Get());
				candidate = candidateLifetime.Get();
				current = currentLifetime.Get();
				parent = parentLifetime.Get();
				if (window != this || !candidate || !current || !parent
					|| candidate->GetAccessibilityRuntimeId()
						!= candidateRuntimeId
					|| current->GetLogicalParent() != parent)
					return nullptr;
				if (peer && peer->SupportsVirtualizedChildren()) return nullptr;
			}
			current = parent;
		}

		candidate = candidateLifetime.Get();
		return windowLifetime.Get() == this && current == this && candidate
			&& candidate->GetAccessibilityRuntimeId() == candidateRuntimeId
			&& candidate->GetPresentationWindow() == this
			? candidate : nullptr;
	}
	catch (...) { return nullptr; }
}

Control* Window::ResolveAccessibleRepresentative(
	Control* control, std::span<Control* const> accessible) const
{
	if (!control) return nullptr;
	std::vector<ControlWeakReference> pending;
	pending.emplace_back(control);
	std::unordered_set<Control*> visited;
	while (!pending.empty())
	{
		const ControlWeakReference candidateLifetime = pending.back();
		pending.pop_back();
		auto* candidate = candidateLifetime.Get();
		if (!candidate || !visited.insert(candidate).second) continue;
		if (std::find(accessible.begin(), accessible.end(), candidate)
			!= accessible.end()) return candidate;
		auto* visualParent = candidate->GetVisualParent();
		auto* logicalParent = candidate->GetLogicalParent();
		auto* templatedParent = candidate->GetTemplatedParent();
		if (visualParent) pending.emplace_back(visualParent);
		if (logicalParent && logicalParent != visualParent)
			pending.emplace_back(logicalParent);
		if (templatedParent && templatedParent != visualParent
			&& templatedParent != logicalParent)
			pending.emplace_back(templatedParent);
	}
	return nullptr;
}

bool Window::TryGetAccessibilityVirtualFocusedNode(
	Control*& owner, uint32_t& virtualId)
{
	owner = nullptr;
	virtualId = 0;
	return Handle && ::IsWindow(Handle) && ::GetFocus() == Handle
		&& _uiaProvider && _uiaProvider->TryGetVirtualFocusedNode(
		GetKeyboardFocusedElement(), owner, virtualId);
}

void Window::RefreshDefaultedButtons()
{
	std::vector<ControlWeakReference> controls;
	std::vector<Control*> pending;
	std::unordered_set<Control*> visited;
	for (auto* root : GetLogicalChildrenView())
		if (root) pending.push_back(root);
	while (!pending.empty())
	{
		auto* control = pending.back();
		pending.pop_back();
		if (!control || !visited.insert(control).second) continue;
		controls.emplace_back(control);
		for (auto* child : control->GetLogicalChildrenView())
			if (child) pending.push_back(child);
	}

	const ControlWeakReference focusedLifetime(
		GetKeyboardFocusedElement());
	for (const auto& reference : controls)
	{
		auto* button = dynamic_cast<Button*>(reference.Get());
		if (!button) continue;
		button->UpdateIsDefaulted(focusedLifetime.Get());
	}
}

void Window::NotifyAccessibilityEvent(Control* control, AccessibilityChange change)
{
	const HWND windowHandle = Handle;
	if (!windowHandle || !::IsWindow(windowHandle)) return;
	if (change == AccessibilityChange::Focus
		&& ::GetFocus() != windowHandle) return;
	const ControlWeakReference windowLifetime(this);
	const ControlWeakReference controlLifetime(control);
	auto* provider = _uiaProvider;
	if (provider) provider->AddRef();
	auto releaseProvider = [](WindowUiaProvider* value)
	{
		if (value) value->Release();
	};
	std::unique_ptr<WindowUiaProvider, decltype(releaseProvider)>
		providerLifetime(provider, releaseProvider);
	auto resolveWindow = [&]() -> Window*
	{
		auto* live = dynamic_cast<Window*>(windowLifetime.Get());
		return live && live->Handle == windowHandle
			&& ::IsWindow(windowHandle) ? live : nullptr;
	};
	auto resolveControl = [&]() -> Control*
	{
		return control ? controlLifetime.Get() : nullptr;
	};
	const bool publishUiaFocusAfterLegacy =
		provider && change == AccessibilityChange::Focus;
	if (provider && !publishUiaFocusAfterLegacy)
	{
		provider->RaiseEvent(control, change);
		if (!resolveWindow() || (control && !resolveControl())) return;
	}
	DWORD eventId = EVENT_OBJECT_STATECHANGE;
	switch (change)
	{
	case AccessibilityChange::Name: eventId = EVENT_OBJECT_NAMECHANGE; break;
	case AccessibilityChange::Description: eventId = EVENT_OBJECT_DESCRIPTIONCHANGE; break;
	case AccessibilityChange::Help: eventId = EVENT_OBJECT_HELPCHANGE; break;
	case AccessibilityChange::Value: eventId = EVENT_OBJECT_VALUECHANGE; break;
	case AccessibilityChange::Focus: eventId = EVENT_OBJECT_FOCUS; break;
	case AccessibilityChange::Structure: eventId = EVENT_OBJECT_REORDER; break;
	case AccessibilityChange::Invoke:
	case AccessibilityChange::Toggle:
	case AccessibilityChange::ExpandCollapse:
	case AccessibilityChange::Selection:
	case AccessibilityChange::SelectionAdded:
	case AccessibilityChange::SelectionRemoved:
	case AccessibilityChange::SelectionSelected:
	case AccessibilityChange::State:
	default: eventId = EVENT_OBJECT_STATECHANGE; break;
	}
	long childId = CHILDID_SELF;
	auto* liveWindow = resolveWindow();
	auto* liveControl = resolveControl();
	if (!liveWindow || (control && !liveControl)) return;
	if (liveControl)
	{
		if (liveWindow->_accessibleObject)
		{
			auto* accessible = liveWindow->_accessibleObject;
			accessible->AddRef();
			auto releaseAccessible = [](WindowAccessibleObject* value)
			{
				if (value) value->Release();
			};
			std::unique_ptr<WindowAccessibleObject,
				decltype(releaseAccessible)> accessibleLifetime(
					accessible, releaseAccessible);
			childId = accessible->ChildIdFor(liveControl);
		}
		else
		{
			auto controls = liveWindow->GetAccessibleControls();
			liveWindow = resolveWindow();
			liveControl = resolveControl();
			if (!liveWindow || !liveControl) return;
			auto* representative = liveWindow->ResolveAccessibleRepresentative(
				liveControl, controls);
			auto position = std::find(
				controls.begin(), controls.end(), representative);
			if (position != controls.end())
				childId = static_cast<long>(position - controls.begin()) + 1;
		}
	}
	liveWindow = resolveWindow();
	liveControl = resolveControl();
	if (!liveWindow || (control && !liveControl)) return;
	if (change == AccessibilityChange::Focus
		&& ::GetFocus() != windowHandle) return;
	::NotifyWinEvent(eventId, windowHandle, OBJID_CLIENT, childId);
	// Keep the MSAA compatibility notification, then let the native fragment
	// event establish the authoritative virtual focus identity for UIA clients.
	if (publishUiaFocusAfterLegacy)
	{
		liveWindow = resolveWindow();
		liveControl = resolveControl();
		if (!liveWindow || (control && !liveControl)
			|| liveWindow->_uiaProvider != provider
			|| !provider->Connected()
			|| ::GetFocus() != windowHandle) return;
		provider->RaiseEvent(liveControl, change);
	}
}

void Window::NotifyAccessibilityVirtualEvent(
	Control* owner, uint32_t virtualId, AccessibilityChange change)
{
	if (!Handle || !::IsWindow(Handle) || !owner || virtualId == 0) return;
	if (change == AccessibilityChange::Focus
		&& ::GetFocus() != Handle) return;
	const ControlWeakReference windowLifetime(this);
	const ControlWeakReference ownerLifetime(owner);
	auto* provider = _uiaProvider;
	if (provider) provider->AddRef();
	auto releaseProvider = [](WindowUiaProvider* value)
	{
		if (value) value->Release();
	};
	std::unique_ptr<WindowUiaProvider, decltype(releaseProvider)>
		providerLifetime(provider, releaseProvider);
	if (provider) provider->RaiseVirtualEvent(owner, virtualId, change);
	auto* liveWindow = dynamic_cast<Window*>(windowLifetime.Get());
	owner = ownerLifetime.Get();
	if (!liveWindow || !owner) return;
	// MSAA retains its compatibility simple-child model; notify the owning child.
	liveWindow->NotifyAccessibilityEvent(owner, change);
}

LRESULT Window::HandleAccessibleObjectRequest(WPARAM wParam, LPARAM lParam)
{
	if (static_cast<LONG>(lParam) == UiaRootObjectId)
	{
		if (!_uiaProvider)
			_uiaProvider = new (std::nothrow) WindowUiaProvider(this);
		if (!_uiaProvider) return 0;
		return ::UiaReturnRawElementProvider(
			Handle, wParam, lParam,
			static_cast<IRawElementProviderSimple*>(_uiaProvider));
	}
	if (static_cast<LONG>(lParam) != OBJID_CLIENT) return 0;
	if (!_accessibleObject)
		_accessibleObject = new (std::nothrow) WindowAccessibleObject(this);
	if (!_accessibleObject) return 0;
	return ::LresultFromObject(
		IID_IAccessible, wParam, static_cast<IAccessible*>(_accessibleObject));
}

bool Window::MoveFocus(bool forward)
{
	return MoveFocus(forward
		? FocusNavigationDirection::Next
		: FocusNavigationDirection::Previous);
}

bool Window::MoveFocus(FocusNavigationDirection direction)
{
	return _focusManager && _focusManager->MoveFocus(direction);
}

bool Window::ProcessAccessKey(wchar_t key)
{
	key = static_cast<wchar_t>(std::towupper(key));
	if (key == L'\0') return false;
	auto focusOrder = _focusManager
		? _focusManager->GetFocusableOrder() : std::vector<Control*>{};
	if (focusOrder.empty()) return false;
	const auto current = std::find(
		focusOrder.begin(), focusOrder.end(), GetKeyboardFocusedElement());
	const size_t start = current == focusOrder.end()
		? 0
		: (static_cast<size_t>(current - focusOrder.begin()) + 1) % focusOrder.size();
	std::vector<Control*> matches;
	for (size_t offset = 0; offset < focusOrder.size(); ++offset)
	{
		auto* candidate = focusOrder[(start + offset) % focusOrder.size()];
		if (!candidate || candidate->GetEffectiveAccessKey() != key) continue;
		matches.push_back(candidate);
	}
	for (auto* candidate : matches)
	{
		if (Handle && ::GetFocus() != Handle)
			::SetFocus(Handle);
		SetKeyboardFocus(candidate, true);
		if (candidate->InvokeAccessKey(matches.size() > 1))
			return true;
	}
	return false;
}

Button* Window::ResolveDialogButton(bool cancel) const
{
	Button* resolved = nullptr;
	auto pending = GetVisualChildrenInZOrder();
	std::unordered_set<Control*> visited;
	while (!pending.empty())
	{
		auto* candidate = pending.back();
		pending.pop_back();
		if (!candidate || !visited.insert(candidate).second)
			continue;

		if (auto* button = dynamic_cast<Button*>(candidate);
			button && button->GetPresentationWindow() == this
			&& (cancel ? button->IsCancel : button->IsDefault))
		{
			const auto snapshot = button->GetAccessibilitySnapshot();
			if (snapshot.Enabled && snapshot.Visible)
			{
				if (resolved && resolved != button)
					return nullptr;
				resolved = button;
			}
		}

		auto children = candidate->GetVisualChildrenInZOrder();
		pending.insert(pending.end(), children.begin(), children.end());
	}
	return resolved;
}

void Window::UpdateMouseOverProjection(
	Control* directlyOver, POINT contentMouse, bool raiseDirectEvents)
{
	if (directlyOver && (directlyOver->GetPresentationWindow() != this
		|| !directlyOver->IsVisible))
		directlyOver = nullptr;
	_mouseOverContentPoint = contentMouse;
	if (_mouseDirectlyOver == directlyOver) return;

	const ControlWeakReference windowLifetime(this);
	_mouseDirectlyOver = directlyOver;
	std::exception_ptr firstError;
	try
		{
		if (_inputManager)
			_inputManager->SetMouseOverOrigin(
				directlyOver, raiseDirectEvents);
	}
	catch (...)
	{
		firstError = std::current_exception();
	}
	if (!windowLifetime.Get())
	{
		if (firstError) std::rethrow_exception(firstError);
		return;
	}
	try { ReconcileMouseDirectlyOverState(); }
	catch (...)
	{
		if (!firstError) firstError = std::current_exception();
	}
	if (firstError) std::rethrow_exception(firstError);
}

void Window::ReconcileMouseDirectlyOverState()
{
	const ControlWeakReference windowLifetime(this);
	const ControlWeakReference requested(_mouseDirectlyOver);
	if (_mouseDirectlyOverStateOwner == requested) return;
	const auto previous = _mouseDirectlyOverStateOwner;
	_mouseDirectlyOverStateOwner = requested;
	std::exception_ptr firstError;
	if (auto* target = previous.Get())
	{
		try
		{
			cui::framework::InputAccess::PublishMouseDirectlyOverState(
				*target, false);
		}
		catch (...)
		{
			firstError = std::current_exception();
		}
		if (!windowLifetime)
		{
			if (firstError) std::rethrow_exception(firstError);
			return;
		}
	}
	if (_mouseDirectlyOverStateOwner == requested)
	{
		if (auto* target = requested.Get())
		{
			try
			{
				cui::framework::InputAccess::PublishMouseDirectlyOverState(
					*target, true);
			}
			catch (...)
			{
				if (!firstError) firstError = std::current_exception();
			}
			if (!windowLifetime)
			{
				if (firstError) std::rethrow_exception(firstError);
				return;
			}
		}
	}
	if (firstError) std::rethrow_exception(firstError);
}

void Window::PublishMouseOverTransition(
	Control& target, bool isMouseOver)
{
	int localX = 0;
	int localY = 0;
	(void)TryGetControlLocalPoint(
		&target, _mouseOverContentPoint, localX, localY);
	auto args = MouseEventArgs(
		MouseButton::None, MouseButtonState::Released,
		0, localX, localY, 0);
	if (isMouseOver)
		target.OnMouseEnter(&target, args);
	else
		target.OnMouseLeave(&target, args);
}

bool Window::TryGetCaptionButtonRect(CaptionButtonKind kind, RECT& out)
{
	if (!HasWindowChrome()) return false;

	// ActualWidth can lag behind the native frame during maximize/restore.
	// Caption rendering and hit testing must follow the current HWND client.
	const auto clientPixels = GetNativeClientSizePixels();
	const float dpiScale = (std::max)(0.001f, GetDpiScale());
	int rightEdge = static_cast<int>(std::floor(
		static_cast<float>(clientPixels.cx) / dpiScale));
	int buttonHeight = GetTitleBarHeightDip();
	int buttonWidth = buttonHeight;

	auto place = [&](CaptionButtonKind k, bool enabled) -> std::optional<RECT>
		{
			if (!enabled) return std::nullopt;
			RECT rect{ rightEdge - buttonWidth, 0, rightEdge, buttonHeight };
			rightEdge -= buttonWidth;
			return rect;
		};

	auto closeR = place(CaptionButtonKind::Close, true);
	auto maxR = place(CaptionButtonKind::Maximize, HasMaximizeBox());
	auto minR = place(CaptionButtonKind::Minimize, HasMinimizeBox());

	auto pick = [&](CaptionButtonKind k) -> std::optional<RECT>
		{
			if (k == CaptionButtonKind::Close) return closeR;
			if (k == CaptionButtonKind::Maximize) return maxR;
			return minR;
		};

	auto rect = pick(kind);
	if (!rect.has_value()) return false;
	out = rect.value();
	return true;
}

bool Window::HitTestCaptionButtons(POINT clientPoint, CaptionButtonKind& outKind)
{
	// clientPoint is in physical pixels (from OS); TryGetCaptionButtonRect returns logical rects.
	const float dpiScale = GetDpiScale();
	POINT logicalPoint{ (LONG)(clientPoint.x / dpiScale), (LONG)(clientPoint.y / dpiScale) };
	RECT rect{};
	if (TryGetCaptionButtonRect(CaptionButtonKind::Close, rect) && PtInRect(&rect, logicalPoint))
	{
		outKind = CaptionButtonKind::Close;
		return true;
	}
	if (TryGetCaptionButtonRect(CaptionButtonKind::Maximize, rect) && PtInRect(&rect, logicalPoint))
	{
		outKind = CaptionButtonKind::Maximize;
		return true;
	}
	if (TryGetCaptionButtonRect(CaptionButtonKind::Minimize, rect) && PtInRect(&rect, logicalPoint))
	{
		outKind = CaptionButtonKind::Minimize;
		return true;
	}
	return false;
}

bool Window::HitTestCaptionButtonResizeExclusion(POINT clientPoint)
{
	if (!HasWindowChrome()) return false;

	const float dpiScale = GetDpiScale();
	POINT logicalPoint{ (LONG)(clientPoint.x / dpiScale), (LONG)(clientPoint.y / dpiScale) };
	const int padding = (std::max)(2, (int)std::ceil((float)GetCustomFrameInset() / dpiScale));

	RECT unionRect{};
	bool hasRect = false;
	const CaptionButtonKind kinds[] = { CaptionButtonKind::Close, CaptionButtonKind::Maximize, CaptionButtonKind::Minimize };
	for (auto kind : kinds)
	{
		RECT rect{};
		if (!TryGetCaptionButtonRect(kind, rect)) continue;
		if (!hasRect)
		{
			unionRect = rect;
			hasRect = true;
		}
		else
		{
			unionRect.left = (std::min)(unionRect.left, rect.left);
			unionRect.top = (std::min)(unionRect.top, rect.top);
			unionRect.right = (std::max)(unionRect.right, rect.right);
			unionRect.bottom = (std::max)(unionRect.bottom, rect.bottom);
		}
	}
	if (!hasRect) return false;

	InflateRect(&unionRect, padding, padding);
	return PtInRect(&unionRect, logicalPoint) != FALSE;
}

void Window::ClearCaptionStates()
{
	_capMinState = CaptionButtonState::None;
	_capMaxState = CaptionButtonState::None;
	_capCloseState = CaptionButtonState::None;
	_capPressed = false;
	_capTracking = false;
}

void Window::UpdateCaptionHover(POINT clientPoint)
{
	if (!HasWindowChrome()) return;
	CaptionButtonKind hit{};
	bool isButtonHovered = HitTestCaptionButtons(clientPoint, hit);

	auto previousMinState = _capMinState;
	auto previousMaxState = _capMaxState;
	auto previousCloseState = _capCloseState;

	_capMinState = (isButtonHovered && hit == CaptionButtonKind::Minimize) ? CaptionButtonState::Hover : CaptionButtonState::None;
	_capMaxState = (isButtonHovered && hit == CaptionButtonKind::Maximize) ? CaptionButtonState::Hover : CaptionButtonState::None;
	_capCloseState = (isButtonHovered && hit == CaptionButtonKind::Close) ? CaptionButtonState::Hover : CaptionButtonState::None;

	if (_capPressed)
	{
		if (_capPressedKind == CaptionButtonKind::Minimize) _capMinState = CaptionButtonState::Pressed;
		if (_capPressedKind == CaptionButtonKind::Maximize) _capMaxState = CaptionButtonState::Pressed;
		if (_capPressedKind == CaptionButtonKind::Close) _capCloseState = CaptionButtonState::Pressed;
	}

	if (previousMinState != _capMinState || previousMaxState != _capMaxState || previousCloseState != _capCloseState)
	{
		RECT titleBarRect = GetTitleBarClientPixelRect();
		Invalidate(titleBarRect, false);
	}
}

void Window::ExecuteCaptionButton(CaptionButtonKind kind)
{
	const ControlWeakReference lifetime(this);
	switch (kind)
	{
	case CaptionButtonKind::Minimize:
		ShowWindow(this->Handle, SW_MINIMIZE);
		break;
	case CaptionButtonKind::Maximize:
		if (!CanResizeWindow())
			break;
		if (IsZoomed(this->Handle))
			ShowWindow(this->Handle, SW_RESTORE);
		else
			ShowWindow(this->Handle, SW_MAXIMIZE);
		break;
	case CaptionButtonKind::Close:
		// Native caption close is queued after the pointer/capture transaction
		// unwinds. Raising Closing synchronously from WM_LBUTTONUP leaves input
		// staging and capture callbacks active while application code opens a
		// modal dialog.
		if (this->Handle)
			(void)::PostMessageW(this->Handle, WM_CLOSE, 0, 0);
		break;
	}
	if (auto* live = dynamic_cast<Window*>(lifetime.Get());
		live && live->Handle)
		live->Invalidate(live->GetTitleBarClientPixelRect(), false);
}


void Window::Invalidate(bool immediate)
{
	if (_layoutDeferral.IsSuspended())
	{
		this->_presentationInvalidated = true;
		_layoutDeferral.QueueFullVisual(immediate);
		return;
	}
	if (!this->Handle) return;
	this->_presentationInvalidated = true;
	if (_renderHost) _renderHost->QueueFullDamage();
	else ::InvalidateRect(this->Handle, nullptr, FALSE);
	ScheduleLayoutDispatch();
	// When the window is disabled/hidden (e.g. during a modal dialog), forcing
	// UpdateWindow can create excessive WM_PAINT churn. Let the system schedule paint.
	if (immediate && ::IsWindowVisible(this->Handle) && ::IsWindowEnabled(this->Handle))
		::UpdateWindow(this->Handle);
}

void Window::Invalidate(const RECT& rect, bool immediate)
{
	if (_layoutDeferral.IsSuspended())
	{
		this->_presentationInvalidated = true;
		_layoutDeferral.QueueVisual(cui::core::Rect::FromLTRB(
			(float)rect.left, (float)rect.top,
			(float)rect.right, (float)rect.bottom), immediate);
		return;
	}
	if (!this->Handle) return;
	this->_presentationInvalidated = true;
	if (_renderHost) _renderHost->QueueDamage(rect);
	else ::InvalidateRect(this->Handle, &rect, FALSE);
	ScheduleLayoutDispatch();
	if (immediate && ::IsWindowVisible(this->Handle) && ::IsWindowEnabled(this->Handle))
		::UpdateWindow(this->Handle);
}

void Window::Invalidate(D2D1_RECT_F rect, bool immediate)
{
	RECT clientRect = ToRECT(rect, 2);
	Invalidate(clientRect, immediate);
}

RECT Window::ContentDipRectToClientPixels(const D2D1_RECT_F& contentRect, float inflateDip) const
{
	float dpiScale = GetDpiScale();
	if (dpiScale <= 0.0f) dpiScale = 1.0f;

	const float padding = (std::max)(0.0f, inflateDip);
	const LONG contentTop = GetTitleBarHeightPixels();

	RECT clientRect{};
	clientRect.left = (LONG)std::floor((contentRect.left - padding) * dpiScale);
	clientRect.top = (LONG)std::floor((contentRect.top - padding) * dpiScale) + contentTop;
	clientRect.right = (LONG)std::ceil((contentRect.right + padding) * dpiScale);
	clientRect.bottom = (LONG)std::ceil((contentRect.bottom + padding) * dpiScale) + contentTop;
	return clientRect;
}

bool Window::RectIntersects(const RECT& a, const RECT& b)
{
	RECT out{};
	return ::IntersectRect(&out, &a, &b) != 0;
}

Window::PresentationBackdropGeometry
Window::ResolvePresentationBackdropGeometry(
	const RECT& logicalDirty,
	const RECT& logicalClient) noexcept
{
	// A region frame clears only the damaged pixels, but the decorative frame
	// always belongs to the client boundary. Drawing a rectangle around
	// logicalDirty turns every pointer-over damage edge into a persistent one-
	// pixel line on transparent retained layers. The already active damage clip
	// limits this stable client-frame geometry to the portion being repainted.
	return PresentationBackdropGeometry{ logicalDirty, logicalClient };
}

RECT Window::ToRECT(D2D1_RECT_F rect, int inflatePx)
{
	RECT result{};
	result.left = (LONG)std::floor(rect.left) - inflatePx;
	result.top = (LONG)std::floor(rect.top) - inflatePx;
	result.right = (LONG)std::ceil(rect.right) + inflatePx;
	result.bottom = (LONG)std::ceil(rect.bottom) + inflatePx;
	return result;
}

void Window::InvalidateControl(Control* control, float inflateDip, bool immediate)
{
	if (!control || !this->Handle) return;
	if (!control->IsVisible) return;
	RECT physicalRect = ContentDipRectToClientPixels(
		control->GetRenderedAbsoluteRectDip(), inflateDip);
	Invalidate(physicalRect, immediate);
}

void Window::SynchronizeActiveDeclarativeAnimationControl(
	Control* control,
	bool hasRetainedRoots) noexcept
{
	const bool shouldRegister = control && hasRetainedRoots
		&& control->GetPresentationWindow() == this;
	if (!shouldRegister
		&& (!control || !control->_registeredDeclarativeAnimationWindow))
		return;
	try
	{
		if (shouldRegister && !control->_registeredDeclarativeAnimationWindow)
		{
			_activeDeclarativeAnimationControls.emplace_back(control);
			control->_registeredDeclarativeAnimationWindow = true;
		}
		else if (!shouldRegister)
		{
			_activeDeclarativeAnimationControls.erase(std::remove_if(
				_activeDeclarativeAnimationControls.begin(),
				_activeDeclarativeAnimationControls.end(),
				[control](const auto& reference)
				{ return !reference.Get() || reference.Get() == control; }),
				_activeDeclarativeAnimationControls.end());
			control->_registeredDeclarativeAnimationWindow = false;
		}
	}
	catch (...)
	{
		// Record registry degradation instead of terminating a noexcept
		// clock-commit path on allocation failure.  The testing seam exposes the
		// degraded scheduler state without restoring a visual-tree fallback.
		_animationRegistryDegraded = true;
	}
	RefreshAnimationTimer();
}

std::vector<Control*> Window::GetRegisteredDeclarativeAnimationControls()
{
	std::vector<Control*> result;
	result.reserve(_activeDeclarativeAnimationControls.size());
	_activeDeclarativeAnimationControls.erase(std::remove_if(
		_activeDeclarativeAnimationControls.begin(),
		_activeDeclarativeAnimationControls.end(),
		[&](const auto& reference)
		{
			auto* live = reference.Get();
			if (!live || live->GetPresentationWindow() != this
				|| !live->HasRetainedVisualStateAnimationRoots())
			{
				if (live && live->GetPresentationWindow() == this)
					live->_registeredDeclarativeAnimationWindow = false;
				return true;
			}
			if (std::find(result.begin(), result.end(), live) != result.end())
				return true;
			live->_registeredDeclarativeAnimationWindow = true;
			result.push_back(live);
			return false;
		}), _activeDeclarativeAnimationControls.end());
	return result;
}

std::vector<Control*>
Window::GetActiveRegisteredDeclarativeAnimationControls()
{
	auto controls = GetRegisteredDeclarativeAnimationControls();
	controls.erase(std::remove_if(
		controls.begin(), controls.end(),
		[](Control* control)
		{
			return !control || !control->IsVisible
				|| !control->HasActiveVisualStateAnimations();
		}), controls.end());
	return controls;
}

void Window::AdvanceRegisteredDeclarativeAnimationControls(
	std::span<Control* const> controls,
	unsigned long long nowMilliseconds,
	bool immediate)
{
	(void)immediate;
	std::vector<ControlWeakReference> lifetimes;
	lifetimes.reserve(controls.size());
	for (auto* control : controls)
		lifetimes.emplace_back(control);
	for (const auto& lifetime : lifetimes)
	{
		auto* control = lifetime.Get();
		if (!control || control->GetPresentationWindow() != this
			|| !control->IsVisible
			|| !control->HasActiveVisualStateAnimations()) continue;
		(void)cui::framework::PresentationAccess::
			AdvanceVisualStateAnimations(*control, nowMilliseconds);
	}
}

void Window::SynchronizeRetainedNativeAnimationControl(
	Control* control,
	bool hasRetainedAnimation) noexcept
{
	const bool shouldRegister = control && hasRetainedAnimation
		&& control->GetPresentationWindow() == this;
	if (!shouldRegister
		&& (!control || !control->_registeredNativeAnimationWindow))
		return;
	try
	{
		if (shouldRegister && !control->_registeredNativeAnimationWindow)
		{
			_retainedNativeAnimationControls.emplace_back(control);
			control->_registeredNativeAnimationWindow = true;
		}
		else if (!shouldRegister)
		{
			_retainedNativeAnimationControls.erase(std::remove_if(
				_retainedNativeAnimationControls.begin(),
				_retainedNativeAnimationControls.end(),
				[control](const auto& reference)
				{ return !reference.Get() || reference.Get() == control; }),
				_retainedNativeAnimationControls.end());
			control->_registeredNativeAnimationWindow = false;
		}
	}
	catch (...)
	{
		_animationRegistryDegraded = true;
	}
	RefreshAnimationTimer();
}

std::vector<Control*> Window::GetRegisteredNativeAnimationControls()
{
	std::vector<Control*> result;
	result.reserve(_retainedNativeAnimationControls.size());
	_retainedNativeAnimationControls.erase(std::remove_if(
		_retainedNativeAnimationControls.begin(),
		_retainedNativeAnimationControls.end(),
		[&](const auto& reference)
		{
			auto* live = reference.Get();
			if (!live || live->GetPresentationWindow() != this
				|| !live->HasRetainedNativeAnimation())
			{
				if (live && live->GetPresentationWindow() == this)
					live->_registeredNativeAnimationWindow = false;
				return true;
			}
			if (std::find(result.begin(), result.end(), live) != result.end())
				return true;
			live->_registeredNativeAnimationWindow = true;
			result.push_back(live);
			return false;
		}), _retainedNativeAnimationControls.end());
	return result;
}

std::vector<Control*> Window::GetActiveRegisteredNativeAnimationControls()
{
	auto controls = GetRegisteredNativeAnimationControls();
	controls.erase(std::remove_if(
		controls.begin(), controls.end(),
		[](Control* control)
		{
			return !control || !control->IsVisible
				|| !control->IsAnimationRunning();
		}), controls.end());
	return controls;
}

void Window::RefreshAnimationTimer() noexcept
{
	auto stopTimer = [&]() noexcept
	{
		if (_animationFrameScheduler)
			_animationFrameScheduler->Stop();
		if (_animationUsesLegacyTimer && _animIntervalMs != 0 && this->Handle)
			::KillTimer(this->Handle, _animTimerId);
		_animationUsesLegacyTimer = false;
		_animIntervalMs = 0;
	};
	try
	{
		const auto handle = this->Handle;
		if (_resourcesCleaned || !handle
			|| ::IsWindowEnabled(handle) == FALSE)
		{
			stopTimer();
			return;
		}

		bool hasActiveAnimation = false;
		UINT desiredIntervalMs = 0;
		auto declarativeControls =
			GetActiveRegisteredDeclarativeAnimationControls();
		if (!declarativeControls.empty())
		{
			hasActiveAnimation = true;
			desiredIntervalMs = 16;
		}
		auto nativeControls = GetActiveRegisteredNativeAnimationControls();
		for (auto* control : nativeControls)
		{
			if (!control) continue;
			hasActiveAnimation = true;
			UINT interval = control->GetAnimationIntervalMs();
			if (interval == 0) interval = 16;
			desiredIntervalMs = desiredIntervalMs == 0
				? interval
				: (std::min)(desiredIntervalMs, interval);
		}

		if (!hasActiveAnimation)
		{
			stopTimer();
			return;
		}

		const bool sourceRunning = _animationUsesLegacyTimer
			|| (_animationFrameScheduler
				&& _animationFrameScheduler->IsRunning());
		if (_animIntervalMs != desiredIntervalMs || !sourceRunning)
		{
			stopTimer();
			bool scheduled = false;
			try
			{
				if (!_animationFrameScheduler)
					_animationFrameScheduler =
						std::make_unique<AnimationFrameScheduler>();
				const auto animationDispatch =
					CuiAnimationFrameDispatchMessage();
				scheduled = animationDispatch != 0u
					&& _animationFrameScheduler->Start(
						handle, animationDispatch, desiredIntervalMs);
			}
			catch (...)
			{
				scheduled = false;
			}
			if (!scheduled)
			{
				if (::SetTimer(
					handle, _animTimerId, desiredIntervalMs, nullptr) == 0)
				{
					_animationRegistryDegraded = true;
					return;
				}
				_animationUsesLegacyTimer = true;
			}
			_animIntervalMs = desiredIntervalMs;
		}
	}
	catch (...)
	{
		stopTimer();
		_animationRegistryDegraded = true;
	}
}

bool Window::IsAnimationFrameSchedulerRunningForTesting() const noexcept
{
	return _animationFrameScheduler
		&& _animationFrameScheduler->IsRunning();
}

bool Window::IsAnimationFramePostedForTesting() const noexcept
{
	return _animationFrameScheduler
		&& _animationFrameScheduler->IsFramePosted();
}

size_t Window::DispatchPendingAnimationInputSlice()
{
	// GetMessage selects registered/posted messages before hardware input.
	// Service a small input slice while the scheduler's posted token is held,
	// then commit one animation frame. This keeps both lanes moving without an
	// unbounded DoEvents-style nested pump.
	constexpr size_t maximumMessages = 8u;
	constexpr double timeBudgetMicroseconds = 2'000.0;
	const ControlWeakReference lifetime(this);
	PresentationFrameTimingClock elapsed;
	size_t dispatched = 0;
	while (dispatched < maximumMessages
		&& (dispatched == 0u
			|| elapsed.TotalMicroseconds() < timeBudgetMicroseconds))
	{
		if (_animationInputSliceHookForTesting)
		{
			if (!_animationInputSliceHookForTesting(
				_animationInputSliceHookContextForTesting)) break;
		}
		else
		{
			MSG input{};
			if (::PeekMessageW(&input, nullptr, 0u, 0u,
				PM_REMOVE | PM_QS_INPUT) == FALSE) break;
			if (input.message == WM_QUIT)
			{
				::PostQuitMessage(static_cast<int>(input.wParam));
				break;
			}
			::TranslateMessage(&input);
			::DispatchMessageW(&input);
			cui::PumpUIThreadCallbacks();
		}
		++dispatched;
		if (!lifetime.Get()) break;
	}
	return dispatched;
}

void Window::InvalidateAnimatedControls(bool immediate)
{
	InvalidateAnimatedControlsAt(::GetTickCount64(), immediate);
}

void Window::InvalidateAnimatedControlsAt(
	unsigned long long nowMilliseconds,
	bool immediate)
{
	// Animation ticks only enqueue retained work. Synchronously entering
	// WM_PAINT from WM_TIMER makes pointer/caption and modal message handling
	// re-enter presentation with half-committed input state.
	(void)immediate;
	auto declarativeControls =
		GetActiveRegisteredDeclarativeAnimationControls();
	auto nativeControls = GetActiveRegisteredNativeAnimationControls();
	std::vector<ControlWeakReference> nativeLifetimes;
	nativeLifetimes.reserve(nativeControls.size());
	for (auto* control : nativeControls)
		nativeLifetimes.emplace_back(control);
	AdvanceRegisteredDeclarativeAnimationControls(
		declarativeControls, nowMilliseconds, immediate);
	for (const auto& lifetime : nativeLifetimes)
	{
		auto* control = lifetime.Get();
		if (!control || control->GetPresentationWindow() != this
			|| !control->IsVisible || !control->IsAnimationRunning())
			continue;
		D2D1_RECT_F rect{};
		if (control->GetAnimatedInvalidRect(rect))
			cui::framework::PresentationAccess::
				InvalidateVisualRect(*control, rect);
		else
			control->InvalidateVisual();
	}
	RefreshAnimationTimer();
}
GET_CPP(Window, float, Left)
{
	return _left;
}

SET_CPP(Window, float, Left)
{
	if (!std::isfinite(value) && !std::isnan(value)) return;
	if (!SetPropertyField(LeftProperty(), _left, value)) return;
	if (_synchronizingNativeBounds || !Handle || !std::isfinite(value)) return;
	RECT bounds{};
	if (!::GetWindowRect(Handle, &bounds)) return;
	const int x = static_cast<int>(std::lround(value * GetDpiScale()));
	(void)::SetWindowPos(Handle, nullptr, x, bounds.top, 0, 0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

GET_CPP(Window, float, Top)
{
	return _top;
}

SET_CPP(Window, float, Top)
{
	if (!std::isfinite(value) && !std::isnan(value)) return;
	if (!SetPropertyField(TopProperty(), _top, value)) return;
	if (_synchronizingNativeBounds || !Handle || !std::isfinite(value)) return;
	RECT bounds{};
	if (!::GetWindowRect(Handle, &bounds)) return;
	const int y = static_cast<int>(std::lround(value * GetDpiScale()));
	(void)::SetWindowPos(Handle, nullptr, bounds.left, y, 0, 0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

SIZE Window::GetNativeClientSizePixels() const noexcept
{
	RECT rect{};
	const HWND handle = _platformHost ? _platformHost->NativeHandle() : nullptr;
	if (!handle || !::GetClientRect(handle, &rect)) return {};
	return SIZE{
		(std::max)(0L, rect.right - rect.left),
		(std::max)(0L, rect.bottom - rect.top) };
}

cui::core::Size Window::GetSpecifiedWindowSizeDip() const noexcept
{
	auto result = GetActualSizeDip();
	if (!(result.width > 0.0f)) result.width = 600.0f;
	if (!(result.height > 0.0f)) result.height = 400.0f;
	const auto& specified = GetSpecifiedLayout();
	if (specified.width.IsFixed()) result.width = specified.width.value;
	if (specified.height.IsFixed()) result.height = specified.height.value;
	return result.NonNegative();
}

void Window::ApplySpecifiedSizeToPlatform()
{
	if (_synchronizingNativeBounds || !Handle) return;
	const auto desired = GetSpecifiedWindowSizeDip();
	const float scale = GetDpiScale();
	const int width = (std::max)(1,
		static_cast<int>(std::lround(desired.width * scale)));
	const int height = (std::max)(1,
		static_cast<int>(std::lround(desired.height * scale)));
	(void)::SetWindowPos(Handle, nullptr, 0, 0, width, height,
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::SynchronizeNativeClientLayoutSlot()
{
	const auto pixels = GetNativeClientSizePixels();
	const float scale = GetDpiScale();
	Control::Arrange(cui::core::Rect{
		0.0f, 0.0f,
		static_cast<float>(pixels.cx) / scale,
		static_cast<float>(pixels.cy) / scale });
}

void Window::SynchronizeNativePosition()
{
	if (!Handle) return;
	RECT bounds{};
	if (!::GetWindowRect(Handle, &bounds)) return;
	const float scale = GetDpiScale();
	_synchronizingNativeBounds = true;
	(void)TrySetCurrentPropertyValue(
		LeftProperty(), BindingValue(static_cast<float>(bounds.left) / scale));
	(void)TrySetCurrentPropertyValue(
		TopProperty(), BindingValue(static_cast<float>(bounds.top) / scale));
	_synchronizingNativeBounds = false;
}

int Window::GetTitleBarHeightPixels() const noexcept
{
	return (std::max)(0,
		static_cast<int>(std::lround(
			GetTitleBarHeightDip() * GetDpiScale())));
}

RECT Window::GetTitleBarClientPixelRect() const noexcept
{
	const auto client = GetNativeClientSizePixels();
	return RECT{ 0, 0, client.cx, GetTitleBarHeightPixels() };
}

cui::core::Size Window::GetContentViewportSizeDip() const noexcept
{
	const auto actual = GetActualSizeDip();
	return cui::core::Size{
		actual.width,
		(std::max)(0.0f, actual.height
			- static_cast<float>(GetTitleBarHeightDip())) };
}
Window::NativeThemeFrame Window::GetNativeThemeFrame() const
{
	NativeThemeFrame theme;
	theme.WindowBackColor = _backcolor;
	theme.WindowForeColor = _forecolor;
	theme.WindowBorderLightColor = this->BorderLightColor;
	theme.WindowBorderDarkColor = this->BorderDarkColor;
	theme.TitleBarBackColor = this->HeadBackColor;
	theme.CaptionHoverColor = this->CaptionHoverColor;
	theme.CaptionPressedColor = this->CaptionPressedColor;
	theme.CloseHoverColor = this->CloseHoverColor;
	theme.ClosePressedColor = this->ClosePressedColor;
	return theme;
}

Window::NativeThemeFrame Window::GetEffectiveNativeThemeFrame() const
{
	auto theme = GetNativeThemeFrame();
	if (!_systemVisualPreferences.HighContrast)
		return theme;
	theme.WindowBackColor = FromSystemColor(COLOR_WINDOW);
	theme.WindowForeColor = FromSystemColor(COLOR_WINDOWTEXT);
	theme.WindowBorderLightColor = FromSystemColor(COLOR_WINDOWTEXT);
	theme.WindowBorderDarkColor = FromSystemColor(COLOR_WINDOWTEXT);
	theme.TitleBarBackColor = FromSystemColor(COLOR_HIGHLIGHT);
	theme.CaptionHoverColor = FromSystemColor(COLOR_HIGHLIGHT);
	theme.CaptionPressedColor = FromSystemColor(COLOR_HOTLIGHT);
	theme.CloseHoverColor = FromSystemColor(COLOR_HIGHLIGHT);
	theme.ClosePressedColor = FromSystemColor(COLOR_HOTLIGHT);
	return theme;
}

D2D1_COLOR_F Window::GetEffectiveControlBackColor(
	D2D1_COLOR_F configured) const
{
	return _systemVisualPreferences.HighContrast
		? FromSystemColor(COLOR_WINDOW) : configured;
}

D2D1_COLOR_F Window::GetEffectiveControlForeColor(
	D2D1_COLOR_F configured) const
{
	return _systemVisualPreferences.HighContrast
		? FromSystemColor(COLOR_WINDOWTEXT) : configured;
}

void Window::ApplySystemVisualPreferences(SystemVisualPreferences preferences)
{
	preferences = Application::NormalizeSystemVisualPreferences(preferences);
	const bool changed =
		preferences.HighContrast != _systemVisualPreferences.HighContrast
		|| preferences.AnimationsEnabled != _systemVisualPreferences.AnimationsEnabled
		|| preferences.KeyboardCuesAlwaysVisible
			!= _systemVisualPreferences.KeyboardCuesAlwaysVisible
		|| preferences.TextScalePercent != _systemVisualPreferences.TextScalePercent;
	if (!changed) return;
	const bool textScaleChanged = preferences.TextScalePercent
		!= _systemVisualPreferences.TextScalePercent;
	_systemVisualPreferences = preferences;
	RefreshKeyboardFocusVisual();
	if (textScaleChanged)
	{
		for (auto* control : GetVisualChildrenView())
			if (control)
				cui::framework::PresentationAccess::
					InvalidateMeasureSubtree(*control);
		RequestLayout();
	}
	if (auto* focused = GetKeyboardFocusedElement()) focused->InvalidateVisual();
	_presentationInvalidated = true;
	Invalidate(false);
	RefreshAnimationTimer();
}

void Window::RefreshSystemVisualPreferences()
{
	ApplySystemVisualPreferences(Application::QuerySystemVisualPreferences());
}

GET_CPP(Window, bool, Topmost)
{
	return (GetWindowLong(this->Handle, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}
SET_CPP(Window, bool, Topmost)
{
	const auto* metadata =
		DependencyPropertyRegistry::GetMetadata(*this, TopmostProperty());
	if (!metadata) return;
	if (_applyingPropertyMetadata != metadata)
	{
		(void)TrySetPropertyValue(
			TopmostProperty(), BindingValue(value),
			DependencyPropertyValueSource::Local);
		return;
	}
	if (value)
	{
		SetWindowPos(this->Handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	}
	else
	{
		SetWindowPos(this->Handle, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	}
}
void Window::OnEffectiveIsEnabledChanged(
	bool previousValue, bool currentValue)
{
	(void)previousValue;
	const HWND handle = this->Handle;
	if (!handle || !::IsWindow(handle)
		|| (::IsWindowEnabled(handle) != FALSE) == currentValue) return;
	// EnableWindow may synchronously dispatch WM_ENABLE/WM_CANCELMODE.  Do not
	// touch this after the call: an application handler may close the Window.
	(void)::EnableWindow(handle, currentValue ? TRUE : FALSE);
}

void Window::OnEffectiveIsVisibleChanged(
	bool previousValue, bool currentValue)
{
	(void)previousValue;
	const HWND handle = this->Handle;
	if (!handle || !::IsWindow(handle)) return;
	::ShowWindow(handle, currentValue ? SW_SHOW : SW_HIDE);
}

::WindowStyle Window::GetWindowStyleValue()
{
	return _windowStyle;
}

void Window::SetWindowStyleValue(::WindowStyle value)
{
	(void)SetPropertyField(WindowStyleProperty(), _windowStyle, value);
}

GET_CPP(Window, ::ResizeMode, ResizeMode)
{
	return _resizeMode;
}

SET_CPP(Window, ::ResizeMode, ResizeMode)
{
	(void)SetPropertyField(ResizeModeProperty(), _resizeMode, value);
}

bool Window::HasWindowChrome() const noexcept
{
	return _windowStyle != ::WindowStyle::None;
}

bool Window::HasMinimizeBox() const noexcept
{
	return HasWindowChrome()
		&& _windowStyle != ::WindowStyle::ToolWindow
		&& _resizeMode != ::ResizeMode::NoResize;
}

bool Window::HasMaximizeBox() const noexcept
{
	return HasWindowChrome()
		&& _windowStyle != ::WindowStyle::ToolWindow
		&& (_resizeMode == ::ResizeMode::CanResize
			|| _resizeMode == ::ResizeMode::CanResizeWithGrip);
}

bool Window::CanResizeWindow() const noexcept
{
	return _resizeMode == ::ResizeMode::CanResize
		|| _resizeMode == ::ResizeMode::CanResizeWithGrip;
}

int Window::GetTitleBarHeightDip() const noexcept
{
	return HasWindowChrome() ? 24 : 0;
}

void Window::SynchronizeNativeWindowStyle()
{
	if (Handle && IsZoomed(Handle) && !CanResizeWindow())
		ShowWindow(Handle, SW_RESTORE);
	SyncNativeWindowStyles(
		Handle, _showInTaskbar, _windowStyle, _resizeMode);
}

GET_CPP(Window, bool, ShowInTaskbar)
{
	return this->_showInTaskbar;
}
SET_CPP(Window, bool, ShowInTaskbar)
{
	const auto* metadata = DependencyPropertyRegistry::GetMetadata(
		*this, ShowInTaskbarProperty());
	if (!metadata) return;
	if (_applyingPropertyMetadata != metadata)
	{
		(void)TrySetPropertyValue(
			ShowInTaskbarProperty(), BindingValue(value),
			DependencyPropertyValueSource::Local);
		return;
	}
	this->_showInTaskbar = value;
	SynchronizeNativeWindowStyle();
}

GET_CPP(Window, std::wstring, Title)
{
	return _title;
}

SET_CPP(Window, std::wstring, Title)
{
	(void)SetPropertyField(TitleProperty(), _title, std::move(value));
}

std::wstring Window::GetSemanticText() const
{
	return _title;
}

namespace
{
	DependencyPropertyOptions<Window, float> WindowPositionOptions(
		float defaultValue CUI_DESIGN_METADATA_ARGUMENTS(int order))
	{
		DependencyPropertyOptions<Window, float> options;
		options.DefaultValue = defaultValue;
		options.Flags = DependencyPropertyFlags::None;
		options.Validate = [](const float& value)
		{
			return std::isfinite(value) || std::isnan(value);
		};
		options.Equals = [](const float& left, const float& right)
		{
			return left == right || (std::isnan(left) && std::isnan(right));
		};
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Layout";
		options.Design.CategoryOrder = 100;
		options.Design.Order = order;
		options.Design.Editor = DependencyPropertyEditorKind::Number;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Step = 0.5;
		)
		return options;
	}

	DependencyPropertyOptions<Window, bool> WindowBooleanOptions(
		bool defaultValue CUI_DESIGN_METADATA_ARGUMENTS(int order))
	{
		DependencyPropertyOptions<Window, bool> options;
		options.DefaultValue = defaultValue;
		options.Flags = DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 300;
		options.Design.Order = order;
		options.Design.Editor = DependencyPropertyEditorKind::Boolean;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		)
		return options;
	}
}

const DependencyProperty& Window::TitleProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Window, std::wstring> options;
		options.DefaultValue = L"Window";
		options.Flags = DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Common";
		options.Design.CategoryOrder = 0;
		options.Design.Order = 10;
		options.Design.Editor = DependencyPropertyEditorKind::Text;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		)
		options.Changed = [](
			Window& target, const std::wstring&, const std::wstring& value)
		{
			target._presentationInvalidated = true;
			if (target.Handle && ::IsWindow(target.Handle))
				::SetWindowTextW(target.Handle, value.c_str());
			target.NotifyAccessibilityEvent(&target, AccessibilityChange::Name);
		};
		return DependencyPropertyRegistry::RegisterStatic<Window, std::wstring>(
			DependencyPropertyRegistrationLiteral(L"Title"),
			[](Window& target) { return target.Title; },
			[](Window& target, const std::wstring& value)
			{ target.Title = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& Window::LeftProperty()
{
	static const auto registration =
		DependencyPropertyRegistry::RegisterStatic<Window, float>(
			DependencyPropertyRegistrationLiteral(L"Left"),
			[](Window& target) { return target.Left; },
			[](Window& target, const float& value) { target.Left = value; }, {},
			WindowPositionOptions(
				(std::numeric_limits<float>::quiet_NaN)()
				CUI_DESIGN_METADATA_ARGUMENTS(10)));
	return *registration;
}

const DependencyProperty& Window::TopProperty()
{
	static const auto registration =
		DependencyPropertyRegistry::RegisterStatic<Window, float>(
			DependencyPropertyRegistrationLiteral(L"Top"),
			[](Window& target) { return target.Top; },
			[](Window& target, const float& value) { target.Top = value; }, {},
			WindowPositionOptions(
				(std::numeric_limits<float>::quiet_NaN)()
				CUI_DESIGN_METADATA_ARGUMENTS(20)));
	return *registration;
}

const DependencyProperty& Window::TopmostProperty()
{
	static const auto registration =
		DependencyPropertyRegistry::RegisterStatic<Window, bool>(
			DependencyPropertyRegistrationLiteral(L"Topmost"),
			[](Window& target) { return target.Topmost; },
			[](Window& target, const bool& value) { target.Topmost = value; }, {},
			WindowBooleanOptions(false CUI_DESIGN_METADATA_ARGUMENTS(20)));
	return *registration;
}

const DependencyProperty& Window::WindowStyleProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Window, ::WindowStyle> options;
		options.DefaultValue = ::WindowStyle::SingleBorderWindow;
		options.Flags = DependencyPropertyFlags::AffectsArrange
			| DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Appearance";
		options.Design.CategoryOrder = 200;
		options.Design.Order = 50;
		options.Design.Editor = DependencyPropertyEditorKind::Choice;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Choices = {
			{ L"None", BindingValue(::WindowStyle::None) },
			{ L"SingleBorderWindow", BindingValue(::WindowStyle::SingleBorderWindow) },
			{ L"ThreeDBorderWindow", BindingValue(::WindowStyle::ThreeDBorderWindow) },
			{ L"ToolWindow", BindingValue(::WindowStyle::ToolWindow) }
		};
		)
		options.Validate = [](const ::WindowStyle& value)
		{
			switch (value)
			{
			case ::WindowStyle::None:
			case ::WindowStyle::SingleBorderWindow:
			case ::WindowStyle::ThreeDBorderWindow:
			case ::WindowStyle::ToolWindow:
				return true;
			default:
				return false;
			}
		};
		options.Changed = [](
			Window& target, const ::WindowStyle&, const ::WindowStyle&)
		{
			target.SynchronizeNativeWindowStyle();
			target.ClearCaptionStates();
			target.RequestLayout();
			target.Invalidate(false);
		};
		return DependencyPropertyRegistry::RegisterStatic<Window, ::WindowStyle>(
			DependencyPropertyRegistrationLiteral(L"WindowStyle"),
			[](Window& target) { return target.WindowStyle; },
			[](Window& target, const ::WindowStyle& value)
			{ target.WindowStyle = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& Window::ResizeModeProperty()
{
	static const auto registration = []
	{
		DependencyPropertyOptions<Window, ::ResizeMode> options;
		options.DefaultValue = ::ResizeMode::CanResize;
		options.Flags = DependencyPropertyFlags::AffectsArrange
			| DependencyPropertyFlags::AffectsRender;
		CUI_DESIGN_METADATA_ONLY(
		options.Design.Category = L"Behavior";
		options.Design.CategoryOrder = 300;
		options.Design.Order = 30;
		options.Design.Editor = DependencyPropertyEditorKind::Choice;
		options.Design.Persistence = DependencyPropertyPersistence::Native;
		options.Design.Choices = {
			{ L"NoResize", BindingValue(::ResizeMode::NoResize) },
			{ L"CanMinimize", BindingValue(::ResizeMode::CanMinimize) },
			{ L"CanResize", BindingValue(::ResizeMode::CanResize) },
			{ L"CanResizeWithGrip", BindingValue(::ResizeMode::CanResizeWithGrip) }
		};
		)
		options.Validate = [](const ::ResizeMode& value)
		{
			switch (value)
			{
			case ::ResizeMode::NoResize:
			case ::ResizeMode::CanMinimize:
			case ::ResizeMode::CanResize:
			case ::ResizeMode::CanResizeWithGrip:
				return true;
			default:
				return false;
			}
		};
		options.Changed = [](
			Window& target, const ::ResizeMode&, const ::ResizeMode&)
		{
			target.SynchronizeNativeWindowStyle();
			target.ClearCaptionStates();
			target.Invalidate(false);
		};
		return DependencyPropertyRegistry::RegisterStatic<Window, ::ResizeMode>(
			DependencyPropertyRegistrationLiteral(L"ResizeMode"),
			[](Window& target) { return target.ResizeMode; },
			[](Window& target, const ::ResizeMode& value)
			{ target.ResizeMode = value; }, {}, std::move(options));
	}();
	return *registration;
}

const DependencyProperty& Window::ShowInTaskbarProperty()
{
	static const auto registration =
		DependencyPropertyRegistry::RegisterStatic<Window, bool>(
			DependencyPropertyRegistrationLiteral(L"ShowInTaskbar"),
			[](Window& target) { return target.ShowInTaskbar; },
			[](Window& target, const bool& value)
			{ target.ShowInTaskbar = value; }, {},
			WindowBooleanOptions(true CUI_DESIGN_METADATA_ARGUMENTS(10)));
	return *registration;
}

void Window::RegisterDependencyProperties()
{
	ContentControl::RegisterDependencyProperties();
#if CUI_ENABLE_DYNAMIC_XAML
	(void)TitleProperty();
	(void)LeftProperty();
	(void)TopProperty();
	(void)TopmostProperty();
	(void)WindowStyleProperty();
	(void)ResizeModeProperty();
	(void)ShowInTaskbarProperty();
#endif
}

Window::Window()
	: ContentControl()
{
	_backcolor = cui::theme::palette::Window;
	_forecolor = cui::theme::palette::TextPrimary;
	RegisterDependencyProperties();
	SetPresentationWindowCore(this);
	_windowBoundsChanged = OnPropertyValueChanged.Subscribe(
		[this](DependencyObject*, const DependencyPropertyChangedEventArgs& args)
		{
			if (args.Property == &Control::WidthProperty()
				|| args.Property == &Control::HeightProperty())
				ApplySpecifiedSizeToPlatform();
		});
	_inputManager = std::make_unique<InputManager>(this);
	_commandManager = std::make_unique<RoutedCommandManager>(*this);
	_textCompositionManager =
		std::make_unique<TextCompositionManager>(*this, *_inputManager);
	_focusManager = std::make_unique<FocusManager>(*this);
	_presentationScene = std::make_unique<PresentationScene>();
	Application::EnsureDpiAwareness();
	// 首个创建 Window 的线程登记为 UI 线程，并建立跨线程封送 dispatcher。
	cui::InitializeUIThread();
	this->_systemVisualPreferences =
		Application::QuerySystemVisualPreferences();

	const auto initialSizeDip = GetSpecifiedWindowSizeDip();
	const SIZE initialSize{
		static_cast<LONG>(std::lround(initialSizeDip.width)),
		static_cast<LONG>(std::lround(initialSizeDip.height)) };
	_platformHost = std::make_unique<PlatformWindowHost>();
	if (!_platformHost->Create(
		this->Title,
		POINT{ 100, 100 },
		initialSize,
		[this](HWND window, UINT message, WPARAM wParam, LPARAM lParam)
		{
			return HandlePlatformWindowMessage(
				window, message, wParam, lParam);
		})) return;
	SynchronizeNativeWindowStyle();

	EnsureDropTargetRegistered();


	Application::RegisterWindow(*this);

	_renderHost = std::make_unique<PresentationRenderHost>();
	(void)_renderHost->Attach(this->Handle, _dpi);
	SynchronizeNativeClientLayoutSlot();
	ClearCaptionStates();
	ApplyWindowIcon();
}

Window::~Window()
{
	CleanupResources();
	if (_platformHost) _platformHost->Destroy();
}

GET_CPP(Window, HWND, Handle)
{
	return _platformHost ? _platformHost->NativeHandle() : nullptr;
}

GET_CPP(Window, Window*, Owner)
{
	return static_cast<Window*>(_owner.Get());
}

SET_CPP(Window, Window*, Owner)
{
	if (value == GetOwner()) return;
	if (_showingAsDialog || (Handle && ::IsWindowVisible(Handle)))
		throw std::logic_error("Window.Owner cannot change while the window is visible");
	if (value == this)
		throw std::invalid_argument("Window cannot own itself");

	for (auto* ancestor = value; ancestor; ancestor = ancestor->GetOwner())
	{
		if (ancestor == this)
			throw std::invalid_argument("Window.Owner cannot create an ownership cycle");
	}

	if (value)
	{
		if (!value->Handle || !::IsWindow(value->Handle))
			throw std::logic_error("Window.Owner must reference an open window");
		if (Handle)
		{
			const DWORD windowThread = ::GetWindowThreadProcessId(Handle, nullptr);
			const DWORD ownerThread =
				::GetWindowThreadProcessId(value->Handle, nullptr);
			if (windowThread != ownerThread)
				throw std::logic_error("Window.Owner must belong to the same UI thread");
		}
	}

	_owner = value;
	if (Handle && ::IsWindow(Handle))
	{
		::SetWindowLongPtrW(Handle, GWLP_HWNDPARENT,
			value ? reinterpret_cast<LONG_PTR>(value->Handle) : 0);
	}
}

GET_CPP(Window, std::optional<bool>, DialogResult)
{
	return _dialogResult;
}

SET_CPP(Window, std::optional<bool>, DialogResult)
{
	if (_dialogResult == value) return;
	if (!value.has_value())
	{
		_dialogResult.reset();
		return;
	}
	if (!_showingAsDialog)
		throw std::logic_error(
			"Window.DialogResult can only be assigned while ShowDialog is active");

	const auto previous = _dialogResult;
	_dialogResult = value;
	const ControlWeakReference lifetime(this);
	Close();
	if (auto* live = static_cast<Window*>(lifetime.Get());
		live && live->_showingAsDialog && live->Handle
		&& ::IsWindow(live->Handle))
	{
		// Closing was cancelled; a result cannot be committed until the dialog closes.
		live->_dialogResult = previous;
	}
}

D2DGraphics* Window::GetCurrentDrawingContext() const noexcept
{
	return _renderHost ? _renderHost->DrawingContext() : nullptr;
}

void Window::CompactTransientPresentations()
{
	const auto previousSize = _transientPresentations.size();
	_transientPresentations.erase(
		std::remove_if(
			_transientPresentations.begin(), _transientPresentations.end(),
			[](const TransientPresentationEntry& entry)
			{ return !entry.Root; }),
		_transientPresentations.end());
	if (_transientPresentations.size() != previousSize)
		InvalidatePresentationStructure();
}

std::vector<Control*> Window::GetTransientPresentationRoots()
{
	CompactTransientPresentations();
	std::vector<Control*> roots;
	roots.reserve(_transientPresentations.size());
	for (const auto& entry : _transientPresentations)
		if (auto* root = entry.Root.Get()) roots.push_back(root);
	return roots;
}

bool Window::OpenTransientPresentation(
	Control* root,
	TransientPresentationOptions options,
	TransientPresentationDismissHandler dismiss)
{
	if (!root || root->GetPresentationWindow() != this || _resourcesCleaned)
		return false;
	const ControlWeakReference windowLifetime(this);
	const ControlWeakReference rootLifetime(root);
	CompactTransientPresentations();

	if (options.CloseExistingDismissiblePresentation)
	{
		for (auto it = _transientPresentations.rbegin();
			it != _transientPresentations.rend(); ++it)
		{
			auto* existing = it->Root.Get();
			if (!existing || existing == root) continue;
			if (!it->Options.DismissOnOutsidePointerDown) break;
			const auto existingLifetime = it->Root;
			const auto existingDismiss = it->Dismiss;
			if (existingDismiss) existingDismiss(*existing);

			auto* liveWindow = dynamic_cast<Window*>(windowLifetime.Get());
			auto* liveRoot = rootLifetime.Get();
			if (!liveWindow || !liveRoot
				|| liveRoot->GetPresentationWindow() != liveWindow) return false;
			if (auto* liveExisting = existingLifetime.Get();
				liveExisting
				&& liveWindow->IsTransientPresentationOpen(liveExisting))
				liveWindow->CloseTransientPresentation(liveExisting);
			options.CloseExistingDismissiblePresentation = false;
			return liveWindow->OpenTransientPresentation(
				liveRoot, options, dismiss);
		}
	}

	bool alreadyOpen = false;
	for (auto it = _transientPresentations.begin();
		it != _transientPresentations.end();)
	{
		if (it->Root.Get() != root)
		{
			++it;
			continue;
		}
		alreadyOpen = true;
		it = _transientPresentations.erase(it);
	}
	_transientPresentations.push_back(TransientPresentationEntry{
		ControlWeakReference(root), options, dismiss });
	if (!alreadyOpen && _focusManager)
		_focusManager->OpenTransientScope(root);
	InvalidatePresentationStructure();
	if (Handle) Invalidate(false);
	return true;
}

bool Window::CloseTransientPresentation(Control* root)
{
	if (!root) return false;
	bool removed = false;
	for (auto it = _transientPresentations.begin();
		it != _transientPresentations.end();)
	{
		auto* current = it->Root.Get();
		if (!current || current == root)
		{
			removed = removed || current == root;
			it = _transientPresentations.erase(it);
		}
		else ++it;
	}
	if (!removed) return false;
	if (_focusManager) _focusManager->CloseTransientScope(root);
	InvalidatePresentationStructure();
	if (!_resourcesCleaned && Handle) Invalidate(false);
	return true;
}

bool Window::IsTransientPresentationOpen(
	const Control* root) const noexcept
{
	if (!root) return false;
	return std::any_of(
		_transientPresentations.begin(), _transientPresentations.end(),
		[root](const TransientPresentationEntry& entry)
		{ return entry.Root.Get() == root; });
}

Control* Window::GetTopmostTransientPresentation() const noexcept
{
	for (auto it = _transientPresentations.rbegin();
		it != _transientPresentations.rend(); ++it)
		if (auto* root = it->Root.Get()) return root;
	return nullptr;
}

size_t Window::GetTransientPresentationCount() const noexcept
{
	return static_cast<size_t>(std::count_if(
		_transientPresentations.begin(), _transientPresentations.end(),
		[](const TransientPresentationEntry& entry)
		{ return static_cast<bool>(entry.Root); }));
}

void Window::DismissTransientPresentationsForPointer(Control* hitControl)
{
	const ControlWeakReference windowLifetime(this);
	const ControlWeakReference hitLifetime(hitControl);
	auto roots = GetTransientPresentationRoots();
	for (auto it = roots.rbegin(); it != roots.rend(); ++it)
	{
		auto* liveWindow = dynamic_cast<Window*>(windowLifetime.Get());
		if (!liveWindow) return;
		auto* root = *it;
		if (!root || !liveWindow->IsTransientPresentationOpen(root)) continue;
		if (auto* hit = hitLifetime.Get();
			hit && IsControlOrDescendantOf(hit, root)) break;
		if (auto* popup = dynamic_cast<Popup*>(root))
		{
			auto* placementTarget = popup->GetPlacementTarget();
			auto* hit = hitLifetime.Get();
			// A placement target and its Popup form one input surface. The
			// owning control decides whether clicking the target toggles the
			// Popup; treating the target as "outside" closes on MouseDown and
			// makes the same click reopen it on MouseUp.
			if (hit && placementTarget
				&& IsControlOrDescendantOf(hit, placementTarget)) break;
		}

		auto entry = std::find_if(
			liveWindow->_transientPresentations.begin(),
			liveWindow->_transientPresentations.end(),
			[root](const TransientPresentationEntry& candidate)
			{ return candidate.Root.Get() == root; });
		if (entry == liveWindow->_transientPresentations.end()
			|| !entry->Options.DismissOnOutsidePointerDown) continue;
		const ControlWeakReference rootLifetime(root);
		const auto dismiss = entry->Dismiss;
		if (dismiss) dismiss(*root);
		liveWindow = dynamic_cast<Window*>(windowLifetime.Get());
		root = rootLifetime.Get();
		if (!liveWindow) return;
		if (root && liveWindow->IsTransientPresentationOpen(root))
			liveWindow->CloseTransientPresentation(root);
	}
}

void Window::DismissTransientPresentationsForWindowDeactivation()
{
	const ControlWeakReference windowLifetime(this);
	auto roots = GetTransientPresentationRoots();
	for (auto it = roots.rbegin(); it != roots.rend(); ++it)
	{
		auto* liveWindow = dynamic_cast<Window*>(windowLifetime.Get());
		if (!liveWindow) return;
		auto* root = *it;
		if (!root || !liveWindow->IsTransientPresentationOpen(root)) continue;
		auto entry = std::find_if(
			liveWindow->_transientPresentations.begin(),
			liveWindow->_transientPresentations.end(),
			[root](const TransientPresentationEntry& candidate)
			{ return candidate.Root.Get() == root; });
		if (entry == liveWindow->_transientPresentations.end()
			|| !entry->Options.DismissOnWindowDeactivation) continue;
		const ControlWeakReference rootLifetime(root);
		const auto dismiss = entry->Dismiss;
		if (dismiss) dismiss(*root);
		liveWindow = dynamic_cast<Window*>(windowLifetime.Get());
		root = rootLifetime.Get();
		if (!liveWindow) return;
		if (root && liveWindow->IsTransientPresentationOpen(root))
			liveWindow->CloseTransientPresentation(root);
	}
}

void Window::DismissTransientPresentationsInSubtree(Control* root)
{
	if (!root) return;
	std::vector<ControlWeakReference> matches;
	for (auto* candidate : GetTransientPresentationRoots())
		if (IsControlOrDescendantOf(candidate, root))
			matches.emplace_back(candidate);
	const ControlWeakReference windowLifetime(this);
	for (const auto& candidateLifetime : matches)
	{
		auto* liveWindow = dynamic_cast<Window*>(windowLifetime.Get());
		auto* candidate = candidateLifetime.Get();
		if (!liveWindow || !candidate) return;
		auto entry = std::find_if(
			liveWindow->_transientPresentations.begin(),
			liveWindow->_transientPresentations.end(),
			[candidate](const TransientPresentationEntry& value)
			{ return value.Root.Get() == candidate; });
		if (entry == liveWindow->_transientPresentations.end()) continue;
		const auto dismiss = entry->Dismiss;
		if (dismiss) dismiss(*candidate);
		liveWindow = dynamic_cast<Window*>(windowLifetime.Get());
		candidate = candidateLifetime.Get();
		if (!liveWindow) return;
		if (candidate && liveWindow->IsTransientPresentationOpen(candidate))
			liveWindow->CloseTransientPresentation(candidate);
	}
}

void Window::ClearTransientPresentations() noexcept
{
	try
	{
		if (_focusManager)
			for (const auto& entry : _transientPresentations)
				if (auto* root = entry.Root.Get())
					_focusManager->CloseTransientScope(root);
		_transientPresentations.clear();
		InvalidatePresentationStructure();
	}
	catch (...)
	{
		_transientPresentations.clear();
	}
}

bool Window::SynchronizePresentationScene()
{
	if (!_presentationScene) return false;
	auto transientRoots = GetTransientPresentationRoots();
	auto animationOwners = GetRegisteredDeclarativeAnimationControls();
	std::vector<Control*> isolationTargets;
	std::vector<Control*> opacityTargets;
	for (auto* owner : animationOwners)
		if (owner)
			owner->CollectDeclarativeCompositionAnimationTargets(
				isolationTargets, opacityTargets);
	isolationTargets.erase(std::remove_if(
		isolationTargets.begin(), isolationTargets.end(),
		[this](Control* target)
		{
			return !target || target->GetPresentationWindow() != this;
		}), isolationTargets.end());
	std::sort(isolationTargets.begin(), isolationTargets.end(),
		std::less<Control*>{});
	isolationTargets.erase(std::unique(
		isolationTargets.begin(), isolationTargets.end()),
		isolationTargets.end());
	opacityTargets.erase(std::remove_if(
		opacityTargets.begin(), opacityTargets.end(),
		[this](Control* target)
		{
			return !target || target->GetPresentationWindow() != this;
		}), opacityTargets.end());
	std::sort(opacityTargets.begin(), opacityTargets.end(),
		std::less<Control*>{});
	opacityTargets.erase(std::unique(
		opacityTargets.begin(), opacityTargets.end()),
		opacityTargets.end());
	return _presentationScene->Synchronize(
		std::span<Control* const>{
			GetVisualChildrenView().data(), GetVisualChildrenView().size() },
		std::span<Control* const>{
			transientRoots.data(), transientRoots.size() },
		std::span<Control* const>{
			isolationTargets.data(), isolationTargets.size() },
		std::span<Control* const>{
			opacityTargets.data(), opacityTargets.size() });
}

void Window::CleanupResources()
{
	if (_resourcesCleaned)
		return;
	_resourcesCleaned = true;
	_layoutDispatchPosted = false;
	if (_animationFrameScheduler)
		_animationFrameScheduler->Stop();
	if (_animationUsesLegacyTimer && _animIntervalMs != 0 && this->Handle)
		::KillTimer(this->Handle, _animTimerId);
	_animationUsesLegacyTimer = false;
	_animIntervalMs = 0;
	_animationInputSliceHookForTesting = nullptr;
	_animationInputSliceHookContextForTesting = nullptr;
	for (const auto& reference : _activeDeclarativeAnimationControls)
		if (auto* control = reference.Get();
			control && control->GetPresentationWindow() == this)
			control->_registeredDeclarativeAnimationWindow = false;
	for (const auto& reference : _retainedNativeAnimationControls)
		if (auto* control = reference.Get();
			control && control->GetPresentationWindow() == this)
			control->_registeredNativeAnimationWindow = false;
	_activeDeclarativeAnimationControls.clear();
	_retainedNativeAnimationControls.clear();
	try { (void)ReleaseMouseCapture(); }
	catch (...) {}
	if (_textCompositionManager) _textCompositionManager->Reset();
	Application::UnregisterWindow(*this);
	if (_uiaProvider)
	{
		_uiaProvider->DetachWindow();
		_uiaProvider->Release();
		_uiaProvider = nullptr;
	}
	if (_accessibleObject)
	{
		_accessibleObject->DetachWindow();
		_accessibleObject->Release();
		_accessibleObject = nullptr;
	}
	if (this->Handle && _dropRegistered)
	{
		RevokeDragDrop(this->Handle);
		_dropRegistered = false;
	}
	if (_dropTarget)
	{
		_dropTarget->Release();
		_dropTarget = nullptr;
	}

	// Transient presentation roots are non-owning projections. Their logical or
	// template owners remain solely responsible for object lifetime.
	ClearTransientPresentations();
	try { UpdateMouseOverProjection(nullptr, POINT{}, false); }
	catch (...) {}
	if (_focusManager) _focusManager->Reset();
	while (!this->GetVisualChildrenView().empty())
	{
		auto owned = DetachVisualChild(this->GetVisualChildrenView().front());
		if (!owned) break;
	}

	if (_renderHost) _renderHost->Detach();
	_renderHost.reset();
}

bool Window::EnsureDCompInitialized()
{
#ifdef CUI_ENABLE_WEBVIEW2
	if (!_renderHost || !_presentationScene) return false;
	// During native-node submission the active transaction already owns a
	// prepared composition tree. Resource discovery must not attempt to allocate
	// scene layers while any surface is inside BeginDraw.
	if (_renderHost->UsesComposition()) return true;
	SynchronizePresentationScene();
	if (!_renderHost->EnsureComposition()) return false;
	// Publish the new device generation before native owners lease visuals from
	// that device during scene preparation. Scene-layer topology may advance the
	// broader cache generation afterwards without invalidating those fresh leases.
	SynchronizePresentationResourceGeneration();
	if (!_presentationScene->PrepareComposition(
		*_renderHost, GetTitleBarHeightDip(), GetDpiScale())) return false;
	SynchronizePresentationResourceGeneration();
	return true;
#else
	return false;
#endif
}

void Window::InvalidatePresentationStructure() noexcept
{
	if (_presentationScene) _presentationScene->InvalidateStructure();
	try
	{
		RefreshDefaultedButtons();
	}
	catch (...)
	{
	}
}

void Window::InvalidatePresentationNode(
	Control* control,
	PresentationInvalidationKind kind) noexcept
{
	if (_presentationScene)
		_presentationScene->InvalidateNode(control, kind);
}

uint64_t Window::GetPresentationSceneRevision() const noexcept
{
	return _presentationScene ? _presentationScene->Revision() : 0;
}

uint64_t Window::GetPresentationContentRevision() const noexcept
{
	return _presentationScene ? _presentationScene->ContentRevision() : 0;
}

uint64_t Window::GetPresentationGeometryRevision() const noexcept
{
	return _presentationScene ? _presentationScene->GeometryRevision() : 0;
}

uint64_t Window::GetPresentationCompositionRevision() const noexcept
{
	return _presentationScene ? _presentationScene->CompositionRevision() : 0;
}

uint64_t Window::GetPresentationResourceGeneration() const noexcept
{
	return _renderHost ? _renderHost->ResourceGeneration() : 0;
}

uint64_t Window::GetPresentationTransactionSequence() const noexcept
{
	return _renderHost ? _renderHost->Statistics().LastSequence : 0;
}

uint64_t Window::GetPresentationCommittedFrameCount() const noexcept
{
	return _renderHost ? _renderHost->Statistics().CommittedFrames : 0;
}

uint64_t Window::GetPresentationAbortedFrameCount() const noexcept
{
	return _renderHost ? _renderHost->Statistics().AbortedFrames : 0;
}

uint64_t Window::GetPresentationDeviceRecoveryCount() const noexcept
{
	return _renderHost ? _renderHost->Statistics().DeviceRecoveries : 0;
}

bool Window::TryGetPresentationSceneLayerPixelDigestForTesting(
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
	return _renderHost
		&& _renderHost->TryGetSceneLayerPixelDigestForTesting(
			index, width, height, digest, nonTransparentPixels);
}

bool Window::AcquirePresentationSceneLayerPixelReadbackLeaseForTesting() noexcept
{
	return _renderHost
		&& _renderHost->AcquireSceneLayerPixelReadbackLeaseForTesting();
}

void Window::ReleasePresentationSceneLayerPixelReadbackLeaseForTesting() noexcept
{
	if (_renderHost)
		_renderHost->ReleaseSceneLayerPixelReadbackLeaseForTesting();
}

uint64_t Window::GetPresentationLastSurfaceFailureSequence() const noexcept
{
	return _renderHost
		? _renderHost->Statistics().LastSurfaceFailureSequence : 0;
}

uint8_t Window::GetPresentationLastFailedSurfaceRole() const noexcept
{
	return _renderHost
		? static_cast<uint8_t>(
			_renderHost->Statistics().LastFailedSurfaceRole) : 0;
}

HRESULT Window::GetPresentationLastFailedEndDrawHr() const noexcept
{
	return _renderHost
		? _renderHost->Statistics().LastFailedEndDrawHr : S_OK;
}

HRESULT Window::GetPresentationLastFailedPresentHr() const noexcept
{
	return _renderHost
		? _renderHost->Statistics().LastFailedPresentHr : S_OK;
}

size_t Window::GetPresentationNodeCount() const noexcept
{
	return _presentationScene ? _presentationScene->NodeCount() : 0;
}

size_t Window::GetPresentationDrawingLayerCount() const noexcept
{
	return _presentationScene ? _presentationScene->DrawingLayerCount() : 0;
}

size_t Window::GetPresentationOpacityGroupCount() const noexcept
{
	return _presentationScene ? _presentationScene->OpacityGroupCount() : 0;
}

size_t Window::GetPresentationGroupedNativeVisualCount() const noexcept
{
	return _renderHost
		? _renderHost->SceneLayerGroupedNativeVisualCountForTesting() : 0;
}

PresentationFrameStatistics
Window::GetPresentationFrameStatistics() const noexcept
{
	return _presentationScene
		? _presentationScene->FrameStatistics()
		: PresentationFrameStatistics{};
}

void Window::InjectPresentationDeviceLossForTesting()
{
	if (!_renderHost) return;
	_renderHost->InjectDeviceLossForTesting();
	Invalidate(false);
}

bool Window::InjectSharedGraphicsDeviceRotationForTesting()
{
	if (!_renderHost) return false;
	GraphicsSharedD3DDeviceInfo replacement{};
	if (FAILED(Graphics_RotateSharedD3DDeviceForTesting(&replacement))
		|| replacement.Generation == 0)
	{
		return false;
	}
	// Every composition host is tied to the previous shared domain.  Queue an
	// OS-level repaint by HWND so windows owned by other dispatchers observe the
	// lock-free generation change on their own threads; never mutate a sibling
	// PresentationRenderHost cross-thread.
	for (HWND handle : Application::GetPlatformWindowHandles())
	{
		if (::IsWindow(handle)) (void)::InvalidateRect(handle, nullptr, FALSE);
	}
	return true;
}
IDCompositionDevice* Window::GetDCompDevice() const
{
#ifdef CUI_ENABLE_WEBVIEW2
	auto* self = const_cast<Window*>(this);
	if (!self->EnsureDCompInitialized()) return nullptr;
	return self->_renderHost
		? self->_renderHost->CompositionDevice() : nullptr;
#else
	return nullptr;
#endif
}

IDCompositionVisual* Window::GetWebContainerVisual() const
{
#ifdef CUI_ENABLE_WEBVIEW2
	auto* self = const_cast<Window*>(this);
	if (!self->EnsureDCompInitialized()) return nullptr;
	return self->_renderHost
		? self->_renderHost->WebContainerVisual() : nullptr;
#else
	return nullptr;
#endif
}

bool Window::RegisterDCompVisual(IDCompositionVisual* visual, int layer, int order)
{
#ifdef CUI_ENABLE_WEBVIEW2
	if ((!_renderHost || !_renderHost->UsesComposition())
		&& !EnsureDCompInitialized()) return false;
	return _renderHost
		? _renderHost->RegisterCompositionVisual(visual, layer, order) : false;
#else
	(void)visual;
	(void)layer;
	(void)order;
	return false;
#endif
}

void Window::UpdateDCompVisualOrder(IDCompositionVisual* visual, int layer, int order)
{
#ifdef CUI_ENABLE_WEBVIEW2
	if (_renderHost)
		_renderHost->UpdateCompositionVisualOrder(visual, layer, order);
#else
	(void)visual;
	(void)layer;
	(void)order;
#endif
}

void Window::UnregisterDCompVisual(IDCompositionVisual* visual)
{
#ifdef CUI_ENABLE_WEBVIEW2
	if (_renderHost) _renderHost->UnregisterCompositionVisual(visual);
#else
	(void)visual;
#endif
}

int Window::GetPresentationOrder(Control* control)
{
	if (!_presentationScene || !control) return 0;
	SynchronizePresentationScene();
	return _presentationScene->GetOrder(control);
}

bool Window::CommitComposition()
{
#ifdef CUI_ENABLE_WEBVIEW2
	return !_renderHost || _renderHost->CommitComposition();
#else
	return true;
#endif
}

void Window::ApplyDpiChange(UINT newDpi)
{
	if (newDpi == 0) newDpi = 96;
	if (this->_dpi == newDpi) return;
	this->_dpi = newDpi;

	// 通过 D2D SetDpi 让渲染引擎在逻辑坐标系（96-DPI 设计值）中工作，
	// 无需再对控件树的位置/大小/字体进行缩放——D2D 内部映射到正确的物理像素。
	if (_renderHost) _renderHost->SetDpi(newDpi);

	const float dpiScale = GetDpiScale();
	std::function<void(Control*)> notifyDpi = [&](Control* control)
	{
		if (!control) return;
		cui::framework::PresentationAccess::NotifyDpiChanged(
			*control, dpiScale);
		for (auto* child : control->GetVisualChildrenView()) notifyDpi(child);
	};
	for (auto* control : GetVisualChildrenView()) notifyDpi(control);

	SynchronizeNativeClientLayoutSlot();
	this->RequestLayout();
	if (_renderHost) _renderHost->InvalidateFrameHistory();
	this->Invalidate(false);
}

void Window::ApplyInitialWindowPosition()
{
	if (!Handle) return;
	RECT bounds{};
	if (!::GetWindowRect(Handle, &bounds)) return;
	const SIZE size{
		bounds.right - bounds.left,
		bounds.bottom - bounds.top };
	const RECT workArea = GetWindowWorkArea(
		Handle, POINT{ bounds.left, bounds.top });
	const float scale = GetDpiScale();
	POINT origin{
		std::isfinite(Left)
			? static_cast<LONG>(std::lround(Left * scale))
			: workArea.left + ((workArea.right - workArea.left) - size.cx) / 2,
		std::isfinite(Top)
			? static_cast<LONG>(std::lround(Top * scale))
			: workArea.top + ((workArea.bottom - workArea.top) - size.cy) / 2 };
	origin = ClampWindowOriginToWorkArea(origin, size, workArea);
	(void)::SetWindowPos(Handle, nullptr, origin.x, origin.y, 0, 0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	SynchronizeNativePosition();
}

void Window::SyncRenderSizeToClient()
{
	if (!this->Handle || !_renderHost) return;
	_renderHost->ResizeToClient();
	_renderHost->SetDpi(_dpi);
}

void Window::EnsureInitialDpiApplied()
{
	if (_initialDpiApplied) return;
	_initialDpiApplied = true;
	if (!this->Handle) return;

	Application::EnsureDpiAwareness();
	UINT dpi = Application::GetDpiForWindow(this->Handle);
	if (dpi == 0) dpi = 96;
	ApplyDpiChange(dpi);
	ApplySpecifiedSizeToPlatform();
	ApplyInitialWindowPosition();
	SyncRenderSizeToClient();
	SynchronizeNativeClientLayoutSlot();
	if (_renderHost) _renderHost->InvalidateFrameHistory();
	this->Invalidate(false);
}

void Window::EnsureOleInitialized()
{
	static bool inited = false;
	if (inited) return;
	inited = true;
	OleInitialize(nullptr);
}

void Window::EnsureDropTargetRegistered()
{
	if (!this->Handle) return;
	if (_dropRegistered) return;
	EnsureOleInitialized();
	if (!_dropTarget)
	{
		_dropTarget = new WindowDropTarget(this);
	}
	HRESULT hr = RegisterDragDrop(this->Handle, _dropTarget);
	if (SUCCEEDED(hr))
		_dropRegistered = true;
}

void Window::SynchronizePresentationResourceGeneration()
{
	if (!_renderHost) return;
	const auto generation = _renderHost->ResourceGeneration();
	if (generation != 0 && generation != _observedResourceGeneration)
	{
		_observedResourceGeneration = generation;
		if (_presentationScene)
			_presentationScene->SynchronizeResourceGeneration(generation);
	}

	const auto deviceGeneration =
		_renderHost->DeviceResourceGeneration();
	if (deviceGeneration == 0
		|| deviceGeneration == _observedDeviceResourceGeneration) return;
	_observedDeviceResourceGeneration = deviceGeneration;

	std::unordered_set<Control*> visited;
	std::function<void(Control*)> invalidateResources =
		[&](Control* control)
	{
		if (!control || !visited.insert(control).second) return;
		cui::framework::PresentationAccess::
			NotifyDeviceResourcesInvalidated(*control);
		for (auto* child : control->GetVisualChildrenView())
			invalidateResources(child);
	};
	for (auto* control : GetVisualChildrenView()) invalidateResources(control);
	for (auto* root : GetTransientPresentationRoots())
		invalidateResources(root);
}

bool Window::RecoverRenderIfNeeded()
{
	if (!_renderHost || !_renderHost->IsDeviceLost()) return false;
	const bool recovered = _renderHost->RecoverDevice();
	if (recovered)
	{
		SynchronizePresentationResourceGeneration();
		this->Invalidate(false);
	}
	return recovered;
}

void Window::RequestLayout()
{
	_contentLayoutPending = true;
	_layoutState.InvalidateMeasure();
	if (_layoutDeferral.IsSuspended())
	{
		_layoutDeferral.QueueLayout();
		return;
	}

	// The root content slot is committed immediately before painting.
	Invalidate(false);
}

void Window::RequestArrangeLayout()
{
	_contentLayoutPending = true;
	_layoutState.InvalidateArrange();
	if (_layoutDeferral.IsSuspended())
	{
		_layoutDeferral.QueueLayout();
		return;
	}
	Invalidate(false);
}

void Window::ScheduleLayoutDispatch()
{
	const HWND handle = this->Handle;
	if (_layoutDispatchPosted || !handle || !::IsWindow(handle)) return;
	const UINT message = CuiPresentationDispatchMessage();
	if (message == 0) return;
	_layoutDispatchPosted = ::PostMessageW(
		handle, message, 0, 0) != FALSE;
}

void Window::BeginWindowLayoutDeferral() noexcept
{
	_layoutDeferral.Suspend();
}

void Window::EndWindowLayoutDeferral(bool performLayout)
{
	const auto work = _layoutDeferral.Resume();
	if (!work.ready)
		return;
	if (work.layoutRequested && !performLayout)
		_contentLayoutPending = true;

	if (work.layoutRequested && performLayout && _contentLayoutPending)
	{
		PerformLayout();
	}

	if (work.layoutRequested || work.fullVisual)
	{
		Invalidate(work.immediate);
		return;
	}

	if (work.visualRequested && !work.visualBounds.IsEmpty())
	{
		const auto bounds = work.visualBounds;
		const RECT rect{
			(LONG)std::floor(bounds.Left()),
			(LONG)std::floor(bounds.Top()),
			(LONG)std::ceil(bounds.Right()),
			(LONG)std::ceil(bounds.Bottom()) };
		Invalidate(rect, work.immediate);
	}
}

void Window::PerformLayout()
{
	const bool ownsInvalidationDeferral = !_layoutDeferral.IsSuspended();
	if (ownsInvalidationDeferral) BeginWindowLayoutDeferral();
	try
	{
		const auto viewport = GetContentViewportSizeDip();
		const float clientWidthDip = viewport.width;
		const float clientHeightDip = viewport.height;
		const auto padding = GetSpecifiedLayout().padding;
		cui::layout::ArrangeOverlayChildren(
			GetLayoutChildrenView(), cui::core::Rect{
				padding.left,
				padding.top,
				(std::max)(0.0f,
					clientWidthDip - padding.Horizontal()),
				(std::max)(0.0f,
					clientHeightDip - padding.Vertical()) });
		_contentLayoutPending = false;
	}
	catch (...)
	{
		if (ownsInvalidationDeferral) EndWindowLayoutDeferral(false);
		throw;
	}
	if (ownsInvalidationDeferral) EndWindowLayoutDeferral(false);
}

void Window::ApplyWindowIcon()
{
	if (!this->Handle) return;
	HICON largeIcon = LoadProcessIcon(false);
	HICON smallIcon = LoadProcessIcon(true);
	if (largeIcon) SendMessage(this->Handle, WM_SETICON, ICON_BIG, (LPARAM)largeIcon);
	if (smallIcon) SendMessage(this->Handle, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
}

void Window::RaiseContentRenderedOnce()
{
	if (this->_contentRenderedRaised) return;
	this->_contentRenderedRaised = true;
	cui::framework::EventAccess::Raise(ContentRendered, this);
}

void Window::Show()
{
	EnsureInitialDpiApplied();
	ApplyWindowIcon();
	if (!this->IsLayoutSuspended() && _contentLayoutPending)
	{
		PerformLayout();
	}
	if (_presentationScene)
	{
		SynchronizePresentationScene();
	}
	if (_presentationScene && _presentationScene->RequiresComposition())
		(void)EnsureDCompInitialized();
	SynchronizeNativeWindowStyle();
	ShowWindow(this->Handle, SW_SHOWNORMAL);
	this->Invalidate(true);
}
static Window* FindActiveCuiWindow(
	Window* exclude,
	const std::vector<Window*>& platformWindows)
{
	auto findWindow = [exclude](HWND handle) -> Window*
	{
		if (!handle) return nullptr;
		auto* candidate = Application::FindWindow(handle);
		return candidate && candidate != exclude && candidate->Handle
			&& ::IsWindow(candidate->Handle)
			&& ::IsWindowVisible(candidate->Handle)
			? candidate : nullptr;
	};

	if (auto* foreground = findWindow(::GetForegroundWindow()))
		return foreground;
	if (auto* active = findWindow(::GetActiveWindow()))
		return active;

	for (auto* window : platformWindows)
	{
		if (auto* candidate = findWindow(window ? window->Handle : nullptr))
			return candidate;
	}

	return nullptr;
}

void Window::PerformPendingLayout()
{
	if (!IsLayoutSuspended() && _contentLayoutPending)
		PerformLayout();
}

std::optional<bool> Window::ShowDialog()
{
	if (_showingAsDialog)
		throw std::logic_error("Window.ShowDialog is already active");
	if (!Handle || !::IsWindow(Handle))
		throw std::logic_error("A closed Window cannot be shown again");
	if (::IsWindowVisible(Handle))
		throw std::logic_error("A visible Window cannot be shown as a dialog");

	EnsureInitialDpiApplied();
	const auto platformWindows = Application::GetPlatformWindows();
	auto* ownerWindow = GetOwner();
	if (!ownerWindow)
		ownerWindow = FindActiveCuiWindow(this, platformWindows);
	HWND ownerHandle = ownerWindow ? ownerWindow->Handle : nullptr;
	if (ownerHandle && !::IsWindow(ownerHandle)) ownerHandle = nullptr;
	::SetWindowLongPtrW(Handle, GWLP_HWNDPARENT,
		ownerHandle ? reinterpret_cast<LONG_PTR>(ownerHandle) : 0);

	const DWORD dialogThread = ::GetWindowThreadProcessId(Handle, nullptr);
	std::vector<HWND> disabledWindows;
	disabledWindows.reserve(platformWindows.size());
	for (auto* window : platformWindows)
	{
		const HWND handle = window ? window->Handle : nullptr;
		if (!window || window == this || !handle || !::IsWindow(handle)) continue;
		if (::GetWindowThreadProcessId(handle, nullptr) != dialogThread) continue;
		if (!::IsWindowVisible(handle) || !::IsWindowEnabled(handle)) continue;
		disabledWindows.push_back(handle);
	}
	for (HWND handle : disabledWindows) ::EnableWindow(handle, FALSE);

	_dialogResult.reset();
	_showingAsDialog = true;

	Show();
	::SetForegroundWindow(Handle);
	::SetActiveWindow(Handle);

	MSG messageRecord{};
	while (::IsWindow(Handle))
	{
		const BOOL result = ::GetMessageW(&messageRecord, nullptr, 0, 0);
		if (result == 0)
		{
			::PostQuitMessage(static_cast<int>(messageRecord.wParam));
			break;
		}
		if (result < 0) break;
		::TranslateMessage(&messageRecord);
		::DispatchMessageW(&messageRecord);
		cui::PumpUIThreadCallbacks();
	}

	_showingAsDialog = false;
	for (HWND handle : disabledWindows)
	{
		if (::IsWindow(handle)) ::EnableWindow(handle, TRUE);
	}
	if (ownerHandle && ::IsWindow(ownerHandle))
	{
		::SetForegroundWindow(ownerHandle);
		::SetActiveWindow(ownerHandle);
	}
	return _dialogResult;
}
void Window::Close()
{
	if (!this->Handle) return;
	(void)::SendMessageW(this->Handle, WM_CLOSE, 0, 0);
}
bool Window::HasPendingRenderWork() const noexcept
{
	if (_presentationInvalidated) return true;
	if (_renderHost
		&& (_renderHost->HasPendingDamage()
			|| _renderHost->NeedsFullFrame())) return true;
	const HWND handle = _platformHost ? _platformHost->NativeHandle() : nullptr;
	RECT pending{};
	return handle && ::GetUpdateRect(handle, &pending, FALSE) != FALSE;
}

bool Window::HasPendingPresentationDamage() const noexcept
{
	return _renderHost && _renderHost->HasPendingDamage();
}

bool Window::TryGetLastRenderDirtyRect(
	RECT& logicalDirty, bool& fullFrame) const noexcept
{
	logicalDirty = {};
	fullFrame = false;
	return _renderHost
		&& _renderHost->TryGetLastPrimaryFrame(logicalDirty, fullFrame);
}

bool Window::UpdateDirtyRect(const RECT& dirty, bool force)
{
	if (!IsWindow(this->Handle) || !_renderHost) return false;

	if (dirty.right <= dirty.left || dirty.bottom <= dirty.top)
		return false;
	if (_renderHost->IsDeviceLost() && !RecoverRenderIfNeeded())
		return false;
	if (!_renderHost->PrimaryContext()) return false;
	PresentationFrameTimingClock timingClock;
	PresentationFrameTimingStatistics frameTiming;

	// Commit the root Content slot before the retained scene reads geometry.
	if (!this->IsLayoutSuspended() && _contentLayoutPending)
	{
		PerformLayout();
	}
	frameTiming.LayoutMicroseconds = timingClock.LapMicroseconds();
	if (!_presentationScene) return false;
	SynchronizePresentationScene();
	frameTiming.SceneSynchronizationMicroseconds =
		timingClock.LapMicroseconds();
	if (!_renderHost->UsesComposition()
		&& _presentationScene->RequiresComposition())
		(void)EnsureDCompInitialized();
	if (_renderHost->UsesComposition())
	{
		bool prepared = _presentationScene->PrepareComposition(
			*_renderHost, GetTitleBarHeightDip(), GetDpiScale());
		if (!prepared && SynchronizePresentationScene())
		{
			// Preparation may invalidate scene structure. Rebuild once before opening
			// frame surfaces so the final animation/resize frame can converge.
			prepared = _presentationScene->PrepareComposition(
				*_renderHost, GetTitleBarHeightDip(), GetDpiScale());
		}
		if (!prepared)
			return false;
	}
	SynchronizePresentationResourceGeneration();
	frameTiming.CompositionPreparationMicroseconds =
		timingClock.LapMicroseconds();
	PresentationRenderHost::FrameTransaction frameTransaction;
	if (!_renderHost->BeginFrameTransaction(
		dirty, force, frameTransaction))
		return false;
	frameTiming.TransactionBeginMicroseconds =
		timingClock.LapMicroseconds();
	struct TransactionScope final
	{
		PresentationRenderHost& Host;
		PresentationRenderHost::FrameTransaction& Transaction;
		~TransactionScope()
		{
			if (Transaction.Open)
				Host.AbortFrameTransaction(Transaction);
		}
	} transactionScope{ *_renderHost, frameTransaction };
	struct DrawingStateScope final
	{
		D2DGraphics* Context = nullptr;
		bool ClipPushed = false;
		~DrawingStateScope()
		{
			if (!Context) return;
			if (ClipPushed)
				Context->PopDrawRect();
			Context->ClearTransform();
		}
	};
	const float dpiSc = frameTransaction.DpiScale;
	const RECT drawRc = frameTransaction.LogicalDirty;
	const RECT logClientRc = frameTransaction.LogicalClient;
	const auto effectiveTheme = GetEffectiveNativeThemeFrame();
	const auto backdrop = ResolvePresentationBackdropGeometry(
		drawRc, logClientRc);

	this->GetDrawingContext()->FillRect(
		(float)backdrop.Damage.left,
		(float)backdrop.Damage.top,
		(float)(backdrop.Damage.right - backdrop.Damage.left),
		(float)(backdrop.Damage.bottom - backdrop.Damage.top),
		effectiveTheme.WindowBackColor);
	this->GetDrawingContext()->DrawRect(
		(float)backdrop.ClientFrame.left,
		(float)backdrop.ClientFrame.top,
		(float)(backdrop.ClientFrame.right - backdrop.ClientFrame.left),
		(float)(backdrop.ClientFrame.bottom - backdrop.ClientFrame.top),
		effectiveTheme.WindowBorderLightColor, 2.0f);
	this->GetDrawingContext()->DrawRect(
		(float)backdrop.ClientFrame.left,
		(float)backdrop.ClientFrame.top,
		(float)(backdrop.ClientFrame.right - backdrop.ClientFrame.left),
		(float)(backdrop.ClientFrame.bottom - backdrop.ClientFrame.top),
		effectiveTheme.WindowBorderDarkColor, 1.0f);

	if (HasWindowChrome())
	{
		const float logW = static_cast<float>(
			logClientRc.right - logClientRc.left);
		const float logH = static_cast<float>(GetTitleBarHeightDip());
		RECT headRc{ 0, 0, (LONG)logW, (LONG)logH };
		if (RectIntersects(drawRc, headRc))
		{
			this->GetDrawingContext()->FillRect(0, 0, logW, logH, effectiveTheme.TitleBarBackColor);
			auto font = this->GetRenderFont();
			float headTextTop = (logH - font->FontHeight) * 0.5f;
			if (headTextTop < 0.0f)
				headTextTop = 0.0f;
			this->GetDrawingContext()->PushDrawRect(0, 0, logW, logH);
			this->GetDrawingContext()->DrawString(
				this->Title, 8.0f, headTextTop,
				effectiveTheme.WindowForeColor, font);

			auto drawBtn = [&](CaptionButtonKind kind, CaptionButtonState st, D2D1_COLOR_F hover, D2D1_COLOR_F pressed)
				{
					RECT r{};
					if (!TryGetCaptionButtonRect(kind, r)) return;
					if (st == CaptionButtonState::Hover)
						this->GetDrawingContext()->FillRect((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), hover);
					else if (st == CaptionButtonState::Pressed)
						this->GetDrawingContext()->FillRect((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), pressed);

					const float left = (float)r.left;
					const float top = (float)r.top;
					const float bw = (float)(r.right - r.left);
					const float bh = (float)(r.bottom - r.top);
					const float s = (bw < bh) ? bw : bh;
					const float cx = left + bw * 0.5f;
					const float cy = top + bh * 0.5f;

					const float icon = s * 0.42f;
					const float half = icon * 0.5f;
					float stroke = s * 0.08f;
					if (stroke < 1.0f) stroke = 1.0f;

					auto drawMinimize = [&]()
						{
							const float y = cy + half * 0.35f;
							this->GetDrawingContext()->DrawLine({ cx - half, y }, { cx + half, y }, effectiveTheme.WindowForeColor, stroke);
						};
					auto drawMaximize = [&]()
						{
							const float x = cx - half;
							const float y = cy - half;
							this->GetDrawingContext()->DrawRect(x, y, icon, icon, effectiveTheme.WindowForeColor, stroke);
						};
					auto drawRestore = [&]()
						{
							const float restoreStroke = (std::min)(stroke, (std::max)(1.0f, s * 0.055f));
							const float total = s * 0.46f;
							const float rect = total * 0.68f;
							const float off = total - rect;
							const float xFront = cx - total * 0.5f;
							const float yFront = cy - total * 0.5f + off;
							const float xBack = xFront + off;
							const float yBack = yFront - off;
							this->GetDrawingContext()->DrawLine({ xBack, yBack }, { xBack + rect, yBack }, effectiveTheme.WindowForeColor, restoreStroke);
							this->GetDrawingContext()->DrawLine({ xBack + rect, yBack }, { xBack + rect, yBack + rect }, effectiveTheme.WindowForeColor, restoreStroke);
							this->GetDrawingContext()->DrawLine({ xBack, yBack }, { xBack, yFront }, effectiveTheme.WindowForeColor, restoreStroke);
							this->GetDrawingContext()->DrawLine({ xFront + rect, yBack + rect }, { xBack + rect, yBack + rect }, effectiveTheme.WindowForeColor, restoreStroke);
							this->GetDrawingContext()->DrawRect(xFront, yFront, rect, rect, effectiveTheme.WindowForeColor, restoreStroke);
						};
					auto drawClose = [&]()
						{
							this->GetDrawingContext()->DrawLine({ cx - half, cy - half }, { cx + half, cy + half }, effectiveTheme.WindowForeColor, stroke);
							this->GetDrawingContext()->DrawLine({ cx + half, cy - half }, { cx - half, cy + half }, effectiveTheme.WindowForeColor, stroke);
						};

					switch (kind)
					{
					case CaptionButtonKind::Minimize:
						drawMinimize();
						break;
					case CaptionButtonKind::Maximize:
						if (IsZoomed(this->Handle))
							drawRestore();
						else
							drawMaximize();
						break;
					case CaptionButtonKind::Close:
						drawClose();
						break;
					}
				};

			drawBtn(CaptionButtonKind::Close, _capCloseState, effectiveTheme.CloseHoverColor, effectiveTheme.ClosePressedColor);
			drawBtn(CaptionButtonKind::Maximize, _capMaxState, effectiveTheme.CaptionHoverColor, effectiveTheme.CaptionPressedColor);
			drawBtn(CaptionButtonKind::Minimize, _capMinState, effectiveTheme.CaptionHoverColor, effectiveTheme.CaptionPressedColor);

			this->GetDrawingContext()->PopDrawRect();
		}
	}
	const int top = GetTitleBarHeightDip();
	const int logContentW = logClientRc.right - logClientRc.left;
	const int logContentH = logClientRc.bottom - logClientRc.top;
	RECT contentDirty = drawRc;
	contentDirty.top -= top;
	contentDirty.bottom -= top;
	if (contentDirty.top < 0) contentDirty.top = 0;
	if (contentDirty.left < 0) contentDirty.left = 0;
	if (contentDirty.right > logContentW) contentDirty.right = logContentW;
	if (contentDirty.bottom > (logContentH - top)) contentDirty.bottom = (logContentH - top);
	frameTiming.PrimarySetupMicroseconds = timingClock.LapMicroseconds();

	if (contentDirty.right > contentDirty.left && contentDirty.bottom > contentDirty.top)
	{
		if (_renderHost->UsesComposition())
		{
			if (!_presentationScene->RenderComposition(
				*_renderHost, frameTransaction, contentDirty, top,
				static_cast<float>(logContentW),
				static_cast<float>(logContentH))) return false;
			if (!_renderHost->OverlayContext())
			{
				auto* drawingContext = this->GetDrawingContext();
				DrawingStateScope drawingState{ drawingContext };
				drawingContext->SetTransform(
					D2D1::Matrix3x2F::Translation(0.0f, (float)top));
				drawingContext->PushDrawRect(
					(float)contentDirty.left, (float)contentDirty.top,
					(float)(contentDirty.right - contentDirty.left),
					(float)(contentDirty.bottom - contentDirty.top));
				drawingState.ClipPushed = true;
				if (!_presentationScene->RenderOverlay(contentDirty)) return false;
			}
		}
		else
		{
			auto* drawingContext = this->GetDrawingContext();
			DrawingStateScope drawingState{ drawingContext };
			drawingContext->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, (float)top));
			drawingContext->PushDrawRect((float)contentDirty.left, (float)contentDirty.top, (float)(contentDirty.right - contentDirty.left), (float)(contentDirty.bottom - contentDirty.top));
			drawingState.ClipPushed = true;

			if (!_presentationScene->RenderRaster(contentDirty)) return false;

			if (!_renderHost->OverlayContext()
				&& !_presentationScene->RenderOverlay(
					contentDirty, true)) return false;
		}
	}
	frameTiming.SceneRenderMicroseconds = timingClock.LapMicroseconds();

	if (!_renderHost->CloseSurface(
		frameTransaction, frameTransaction.Primary)) return false;

	PresentationRenderHost::SurfaceFrame overlayFrame;
	if (_renderHost->OverlayContext()
		&& contentDirty.right > contentDirty.left
		&& contentDirty.bottom > contentDirty.top
		&& _presentationScene->RequiresOverlayFrame())
	{
		if (!_renderHost->OpenOverlaySurface(
			frameTransaction, overlayFrame)) return false;
		auto* overlayRender = overlayFrame.Context;
		const float ovLogW = static_cast<float>(
			frameTransaction.LogicalClient.right
			- frameTransaction.LogicalClient.left);
		const float ovLogH = static_cast<float>(
			frameTransaction.LogicalClient.bottom
			- frameTransaction.LogicalClient.top);

		const int ovTop = GetTitleBarHeightDip();
		RECT overlayContent = contentDirty;
		overlayContent.right = (std::min)(
			overlayContent.right, static_cast<LONG>(ovLogW));
		overlayContent.bottom = (std::min)(
			overlayContent.bottom, static_cast<LONG>(ovLogH) - ovTop);

		if (overlayContent.right > overlayContent.left && overlayContent.bottom > overlayContent.top)
		{
			DrawingStateScope drawingState{ overlayRender };
			overlayRender->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, (float)ovTop));
			overlayRender->PushDrawRect((float)overlayContent.left, (float)overlayContent.top, (float)(overlayContent.right - overlayContent.left), (float)(overlayContent.bottom - overlayContent.top));
			drawingState.ClipPushed = true;

			if (!_presentationScene->RenderOverlay(overlayContent)) return false;
		}

		if (!_renderHost->CloseSurface(
			frameTransaction, overlayFrame)) return false;
	}
	frameTiming.SurfaceFinalizeMicroseconds = timingClock.LapMicroseconds();

	if (!_renderHost->CommitFrameTransaction(frameTransaction)) return false;
	frameTiming.CompositionCommitMicroseconds = timingClock.LapMicroseconds();
	frameTiming.TotalMicroseconds = timingClock.TotalMicroseconds();
	_presentationScene->SetFrameTimingStatistics(frameTiming);
	this->_presentationInvalidated = _renderHost->HasPendingDamage();
	RefreshAnimationTimer();
	if (!_contentRenderedRaised && Handle && ::IsWindowVisible(Handle))
		RaiseContentRenderedOnce();
	return true;
}

void Window::RefreshReverseInheritedInputProperties()
{
	if (!_resourcesCleaned && _inputManager)
		_inputManager->RefreshReverseInheritedProperties();
}

UINT Window::GetPresentationDispatchMessageForTesting() noexcept
{
	return CuiPresentationDispatchMessage();
}

void Window::ClearDetachedControlReferences(Control* root)
{
	if (!root)
		return;
	if (_textCompositionManager) _textCompositionManager->DetachVisualChild(root);
	auto belongsToDetachedTree = [root](Control* control)
	{
		return IsControlOrDescendantOf(control, root);
	};

	DismissTransientPresentationsInSubtree(root);
	if (belongsToDetachedTree(this->_mouseDirectlyOver))
		UpdateMouseOverProjection(
			nullptr, POINT{}, !_resourcesCleaned);
	if (belongsToDetachedTree(this->GetKeyboardFocusedElement()))
		this->SetKeyboardFocus(
			nullptr, true, FocusChangeReason::TreeDetach);
	if (_focusManager) _focusManager->DetachVisualChild(root);
	if (_inputManager) _inputManager->DetachVisualChild(*this, root);
}

bool Window::ProcessInput(const InputReport& input)
{
	RecordMostRecentInputDevice(input);
	const bool isKeyDown = input.Kind == InputReportKind::KeyDown;
	const bool isKeyUp = input.Kind == InputReportKind::KeyUp;
	if (!isKeyDown && !isKeyUp)
	{
		(void)OnPreviewInputReport(input);
		switch (input.Kind)
		{
		case InputReportKind::FocusGained:
			if (_focusManager) (void)_focusManager->ActivateWindow();
			if (auto* focused = GetKeyboardFocusedElement())
				(void)cui::framework::InputAccess::DispatchInput(
					*focused, input);
			return true;
		case InputReportKind::FocusLost:
			if (auto* focused = GetKeyboardFocusedElement())
				(void)cui::framework::InputAccess::DispatchInput(
					*focused, input);
			if (_focusManager) _focusManager->DeactivateWindow();
			// WPF mouse-device deactivation retires capture before the
			// presentation source becomes inactive.
			(void)ReleaseMouseCapture();
			DismissTransientPresentationsForWindowDeactivation();
			return true;
		case InputReportKind::Cancel:
		{
			auto* target = GetMouseCaptured();
			if (!target) target = GetKeyboardFocusedElement();
			return target
				? cui::framework::InputAccess::DispatchInput(*target, input)
				: false;
		}
		default:
			return Control::ProcessInput(input);
		}
	}

	if (OnPreviewInputReport(input))
	{
		_lastKeyboardMessageHandled = true;
		return true;
	}

	auto* focused = GetKeyboardFocusedElement();
	const ControlWeakReference focusedLifetime(focused);
	auto* originalSource = focused ? focused : this;
	auto eventArgs = input.CreateKeyEventArgs();
	InputManager::StagingScope staging(
		*_inputManager, originalSource,
		isKeyDown ? RoutedEventId::KeyDown : RoutedEventId::KeyUp);
	staging.Preview(eventArgs);
	// Preview handlers may commit/cancel a DataGrid edit and synchronously
	// destroy the focused editor.  Keep the original input target weak from the
	// moment it is sampled, then retire the raw pointer before any later routing.
	focused = focusedLifetime.Get();
	bool handled = staging.Handled();

	if (isKeyUp)
	{
		if (!handled && focused)
		{
			handled = cui::framework::InputAccess::DispatchInput(
				*focused, input);
			if (handled)
			{
				auto args = input.CreateKeyEventArgs();
				OnKeyUp(this, args);
			}
		}
		else if (!handled)
		{
			auto args = input.CreateKeyEventArgs();
			OnKeyUp(this, args);
		}
		if (handled) eventArgs.Handled = true;
		staging.Complete(eventArgs);
		handled = handled || staging.Handled();
		_lastKeyboardMessageHandled = handled;
		return handled;
	}

	const Key key = input.Key;
	auto suppressTranslatedCharacter = [&]
	{
		if (!_textCompositionManager) return;
		if (eventArgs.HasModifier(ModifierKeys::Control) && key >= Key::A && key <= Key::Z)
			_textCompositionManager->SuppressNextCharacter(
				static_cast<wchar_t>(static_cast<int>(key)
					- static_cast<int>(Key::A) + 1));
		else if (key == Key::Return)
			_textCompositionManager->SuppressNextCharacter(L'\r');
		else if (key == Key::Tab)
			_textCompositionManager->SuppressNextCharacter(L'\t');
		else if (key == Key::Space)
			_textCompositionManager->SuppressNextCharacter(L' ');
		else if (key == Key::Escape)
			_textCompositionManager->SuppressNextCharacter(L'\x1b');
	};

	if (!handled)
	{
		auto* commandTarget = focused ? focused : this;
		if (RoutedCommandManager::ProcessInput(*commandTarget, eventArgs))
		{
			eventArgs.Handled = true;
			handled = true;
		}
		// A command binding (for example DataGrid's Escape cancellation) may
		// replace and destroy the editor that was focused when this report began.
		focused = focusedLifetime.Get();
	}

	auto* ancestorNavigationTarget =
		GetAncestorNavigationFallbackTarget(focused, key);
	const ControlWeakReference ancestorNavigationLifetime(
		ancestorNavigationTarget);
	const bool selectedHandles = focused
		&& (focused->HandlesNavigationKey(key)
			|| ancestorNavigationTarget != nullptr);
	if (!handled && key == Key::Tab && !selectedHandles)
	{
		handled = MoveFocus(!input.HasModifier(ModifierKeys::Shift));
		if (handled)
		{
			auto args = input.CreateKeyEventArgs();
			OnKeyDown(this, args);
		}
	}
	if (!handled && !selectedHandles)
	{
		std::optional<FocusNavigationDirection> direction;
		switch (key)
		{
		case Key::Left: direction = FocusNavigationDirection::Left; break;
		case Key::Right: direction = FocusNavigationDirection::Right; break;
		case Key::Up: direction = FocusNavigationDirection::Up; break;
		case Key::Down: direction = FocusNavigationDirection::Down; break;
		default: break;
		}
		if (direction && MoveFocus(*direction))
		{
			handled = true;
			auto args = input.CreateKeyEventArgs();
			OnKeyDown(this, args);
		}
	}

	Button* invocationTarget = nullptr;
	if (!handled && !selectedHandles && key == Key::Escape)
		invocationTarget = ResolveDialogButton(true);
	else if (!handled && !selectedHandles && key == Key::Return)
	{
		auto* focusedButton = dynamic_cast<Button*>(focused);
		if (focusedButton)
		{
			const auto snapshot = focusedButton->GetAccessibilitySnapshot();
			if (snapshot.Enabled && snapshot.Visible)
				invocationTarget = focusedButton;
		}
		if (!invocationTarget) invocationTarget = ResolveDialogButton(false);
	}
	else if (!handled && !selectedHandles && key == Key::Space)
	{
		auto* focusedButton = dynamic_cast<Button*>(focused);
		if (focusedButton)
		{
			const auto snapshot = focusedButton->GetAccessibilitySnapshot();
			if (snapshot.Enabled && snapshot.Visible)
				invocationTarget = focusedButton;
		}
	}

	const auto invocationSnapshot = invocationTarget
		? invocationTarget->GetAccessibilitySnapshot()
		: AccessibilitySnapshot{};
	if (invocationTarget && invocationTarget->GetPresentationWindow() == this
		&& invocationSnapshot.Enabled && invocationSnapshot.Visible)
	{
		handled = true;
		if (focused)
		{
			auto args = input.CreateKeyEventArgs();
			focused->OnKeyDown(focused, args);
		}
		auto args = input.CreateKeyEventArgs();
		OnKeyDown(this, args);
		if (!input.IsRepeat) (void)invocationTarget->Invoke();
	}

	if (!handled && focused)
	{
		handled = cui::framework::InputAccess::DispatchInput(
			*focused, input);
		if (handled)
		{
			auto args = input.CreateKeyEventArgs();
			OnKeyDown(this, args);
		}
		else
		{
			auto* fallbackTarget = dynamic_cast<Control*>(
				ancestorNavigationLifetime.Get());
			auto* liveFocused = dynamic_cast<Control*>(focusedLifetime.Get());
			if (!fallbackTarget && liveFocused)
				fallbackTarget = GetAncestorNavigationFallbackTarget(
					liveFocused, key);
			if (fallbackTarget)
			{
				handled = cui::framework::InputAccess::DispatchInput(
					*fallbackTarget, input);
				if (handled)
				{
					auto args = input.CreateKeyEventArgs();
					OnKeyDown(this, args);
				}
			}
		}
	}
	else if (!handled && !focused)
	{
		auto args = input.CreateKeyEventArgs();
		OnKeyDown(this, args);
	}

	if (handled) eventArgs.Handled = true;
	staging.Complete(eventArgs);
	handled = handled || staging.Handled();
	if (handled) suppressTranslatedCharacter();
	_lastKeyboardMessageHandled = handled;
	return handled;
}

bool Window::OnPreviewInputReport(const InputReport& input)
{
	(void)input;
	return false;
}

std::optional<LRESULT> Window::OnPlatformMessage(
	UINT message, WPARAM wParam, LPARAM lParam)
{
	(void)message;
	(void)wParam;
	(void)lParam;
	return std::nullopt;
}

void Window::ProcessPlatformMessage(
	UINT message, WPARAM wParam, LPARAM lParam)
{
	_lastKeyboardMessageHandled = false;
	_hasDirectMessageResult = false;
	_directMessageResult = 0;
	const bool focusLifecycleMessage = message == WM_SETFOCUS
		|| message == WM_KILLFOCUS;
	if ((!this->IsEnabled || !this->IsVisible) && !focusLifecycleMessage) return;
	if (_textCompositionManager)
	{
		const auto textResult = _textCompositionManager->ProcessWindowMessage(
			message, wParam, lParam);
		if (textResult.Recognized)
		{
			if (!textResult.CallDefaultWindowProcedure)
			{
				_hasDirectMessageResult = true;
				_directMessageResult = textResult.Result;
			}
			return;
		}
	}
	POINT mouse{};
	if (!::GetCursorPos(&mouse) || !::ScreenToClient(this->Handle, &mouse))
		mouse = {};
	switch (message)
	{
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
	case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
		mouse = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		break;
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		mouse = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		(void)::ScreenToClient(this->Handle, &mouse);
		break;
	default:
		break;
	}
	const int titleBarHeight = GetTitleBarHeightPixels();
	const float dpiScale = GetDpiScale();
	POINT contentMouse{ (LONG)(mouse.x / dpiScale), (LONG)((mouse.y - titleBarHeight) / dpiScale) };
	auto nativeInput = CreateNativeInputReport(
		message, wParam, lParam, contentMouse);
	if (nativeInput) RecordMostRecentInputDevice(*nativeInput);
	const bool previewablePointerInput = nativeInput
		&& (nativeInput->Kind == InputReportKind::PointerMove
			|| nativeInput->Kind == InputReportKind::PointerDown
			|| nativeInput->Kind == InputReportKind::PointerUp
			|| nativeInput->Kind == InputReportKind::PointerDoubleClick
			|| nativeInput->Kind == InputReportKind::MouseWheel
			|| nativeInput->Kind == InputReportKind::HorizontalMouseWheel);
	const bool windowChromePointerInput = previewablePointerInput
		&& HasWindowChrome() && mouse.y < titleBarHeight;
	if (previewablePointerInput && !windowChromePointerInput
		&& OnPreviewInputReport(*nativeInput))
	{
		return;
	}
	Control* stagedHitControl = nullptr;
	std::optional<InputManager::StagingScope> inputStaging;
	std::optional<MouseEventArgs> stagedMouseArgs;
	bool stagedInputBindingHandled = false;
	RoutedEventId stagedEvent = RoutedEventId::None;
	if (nativeInput) switch (nativeInput->Kind)
	{
	case InputReportKind::MouseWheel:
	case InputReportKind::HorizontalMouseWheel:
		stagedEvent = RoutedEventId::MouseWheel; break;
	case InputReportKind::PointerMove:
		stagedEvent = RoutedEventId::MouseMove; break;
	case InputReportKind::PointerDown:
		stagedEvent = RoutedEventId::MouseDown; break;
	case InputReportKind::PointerUp:
		stagedEvent = RoutedEventId::MouseUp; break;
	case InputReportKind::PointerDoubleClick:
		stagedEvent = RoutedEventId::MouseDoubleClick; break;
	default: break;
	}
	if (!windowChromePointerInput
		&& GetRoutedEventMetadata(stagedEvent).Device
		== RoutedInputDeviceKind::Mouse)
	{
		auto* captured = GetMouseCaptured();
		// Capture fixes OriginalSource for the duration of the native drag.  Do
		// not compute an unused hit target underneath it; DataGrid headers and
		// ScrollViewer thumbs both generate dense WM_MOUSEMOVE streams.
		if ((!captured || !captured->IsVisible
			|| nativeInput->Kind != InputReportKind::PointerMove)
			&& !(this->HasWindowChrome() && mouse.y < titleBarHeight))
			stagedHitControl = HitTestControlAt(contentMouse);
		auto* originalSource = captured && captured->IsVisible
			? captured
			: (stagedHitControl ? stagedHitControl : this);
		stagedMouseArgs.emplace(nativeInput->CreateMouseEventArgs());
		inputStaging.emplace(*_inputManager, originalSource, stagedEvent,
			static_cast<float>(contentMouse.x),
			static_cast<float>(contentMouse.y));
		inputStaging->Preview(*stagedMouseArgs);
		if (!inputStaging->Handled())
		{
			stagedInputBindingHandled = RoutedCommandManager::ProcessInput(
				*originalSource, *stagedMouseArgs, nativeInput->Modifiers);
			if (stagedInputBindingHandled) stagedMouseArgs->Handled = true;
		}
	}
	Control* hitControl = nullptr;
	auto forwardToCapturedControl = [&]() -> bool
		{
			if (!nativeInput) return false;
			auto* captured = GetMouseCaptured();
			if (!captured || !captured->IsVisible)
				return false;
			int localX = 0;
			int localY = 0;
			if (!TryGetControlLocalPoint(
				captured, contentMouse, localX, localY, false))
				return false;
			hitControl = captured;
			(void)cui::framework::InputAccess::DispatchInput(
				*captured, nativeInput->Retarget(localX, localY));
			return true;
		};
	switch (message)
	{
	case WM_MOUSEMOVE:
	{
		if (!this->_mouseLeaveTracking && this->Handle)
		{
			TRACKMOUSEEVENT tme{};
			tme.cbSize = sizeof(tme);
			tme.dwFlags = TME_LEAVE;
			tme.hwndTrack = this->Handle;
			::TrackMouseEvent(&tme);
			this->_mouseLeaveTracking = true;
		}

		if (this->HasWindowChrome()
			&& mouse.y < GetTitleBarHeightPixels())
		{
			UpdateCaptionHover(mouse);
		}
		else if (this->_capMinState != CaptionButtonState::None || this->_capMaxState != CaptionButtonState::None || this->_capCloseState != CaptionButtonState::None)
		{
			if (!this->_capPressed)
			{
				ClearCaptionStates();
				Invalidate(GetTitleBarClientPixelRect(), false);
			}
		}

		if (this->HasWindowChrome() && mouse.y < titleBarHeight)
		{
			if (GetMouseCaptured() && forwardToCapturedControl())
			{
				UpdateCursor(mouse, contentMouse);
				break;
			}
			UpdateMouseOverProjection(nullptr, contentMouse);
			this->OnMouseMove(this, MouseEventArgs(
				MouseButton::None, MouseButtonState::Released,
				0, contentMouse.x, contentMouse.y, 0));
			ApplyCursor(CursorKind::Arrow);
			break;
		}

		if (GetMouseCaptured() && forwardToCapturedControl())
		{
			UpdateCursor(mouse, contentMouse);
			break;
		}

		Control* newHover = stagedHitControl;
		UpdateMouseOverProjection(newHover, contentMouse);

		auto* hit = stagedHitControl ? stagedHitControl : this;
		if (hit)
		{
			if (hit != this) hitControl = hit;
			int localX = 0;
			int localY = 0;
			if (TryGetControlLocalPoint(hit, contentMouse, localX, localY))
				(void)cui::framework::InputAccess::DispatchInput(
					*hit, nativeInput->Retarget(localX, localY));
		}
		UpdateCursor(mouse, contentMouse);
	}
	break;
	case WM_MOUSELEAVE:
	{
		this->_mouseLeaveTracking = false;
		if (GetMouseCaptured())
		{
			UpdateCursorFromCurrentMouse();
			break;
		}
		if (this->_mouseDirectlyOver && nativeInput)
		{
			int hoverX = 0;
			int hoverY = 0;
			if (TryGetControlLocalPoint(
				this->_mouseDirectlyOver, contentMouse, hoverX, hoverY))
				(void)cui::framework::InputAccess::DispatchInput(
					*this->_mouseDirectlyOver,
					nativeInput->Retarget(hoverX, hoverY));
		}
		UpdateMouseOverProjection(nullptr, contentMouse);
		UpdateCursorFromCurrentMouse();
	}
	break;
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
	case WM_XBUTTONUP:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDBLCLK:
	case WM_XBUTTONDBLCLK:
	{
		if (inputStaging
			&& (inputStaging->Handled() || stagedInputBindingHandled)) break;
		Control* pointerHover = nullptr;
		if (!(this->HasWindowChrome() && mouse.y < titleBarHeight))
		{
			pointerHover = stagedHitControl;
		}
		UpdateMouseOverProjection(pointerHover, contentMouse);

		if (nativeInput->Kind == InputReportKind::PointerDown)
			DismissTransientPresentationsForPointer(pointerHover);

		if (nativeInput->Kind == InputReportKind::PointerDown)
		{
			if (!(this->HasWindowChrome() && mouse.y < titleBarHeight))
			{
				if (::GetFocus() != this->Handle)
					::SetFocus(this->Handle);
			}
		}

		if (WM_LBUTTONDOWN == message)
		{
			if (HasWindowChrome())
			{
				CaptionButtonKind kind{};
				if (HitTestCaptionButtons(mouse, kind))
				{
					_capPressed = true;
					_capPressedKind = kind;
					_capTracking = true;
					UpdateCaptionHover(mouse);
					(void)ReleaseMouseCapture();
					::SetCapture(this->Handle);
					break;
				}

				if (mouse.y < titleBarHeight)
				{
					(void)ReleaseMouseCapture();
					::ReleaseCapture();
					PostMessage(this->Handle, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
				}
			}
		}
		else if (WM_LBUTTONUP == message)
		{
			if (_capTracking)
			{
				::ReleaseCapture();
				_capTracking = false;
				CaptionButtonKind kind{};
				bool hit = HitTestCaptionButtons(mouse, kind);
				if (_capPressed && hit && kind == _capPressedKind)
				{
					_capPressed = false;
					ClearCaptionStates();
					ExecuteCaptionButton(kind);
					UpdateCursor(mouse, contentMouse);
					break;
				}
				_capPressed = false;
				ClearCaptionStates();
				Invalidate(GetTitleBarClientPixelRect(), false);
				UpdateCursor(mouse, contentMouse);
				break;
			}

		}
		else if (nativeInput->Kind == InputReportKind::PointerDoubleClick)
		{
			if (HasWindowChrome() && mouse.y < GetTitleBarHeightPixels())
			{
				CaptionButtonKind kind{};
				if (!HitTestCaptionButtons(mouse, kind))
				{
					ExecuteCaptionButton(CaptionButtonKind::Maximize);
					break;
				}
			}
		}
		if (this->HasWindowChrome() && mouse.y < titleBarHeight)
		{
			if (nativeInput->Kind == InputReportKind::PointerUp &&
				GetMouseCaptured() && forwardToCapturedControl())
			{
				UpdateCursor(mouse, contentMouse);
				break;
			}
			break;
		}

		if ((nativeInput->Kind == InputReportKind::MouseWheel
			|| nativeInput->Kind == InputReportKind::HorizontalMouseWheel
			|| nativeInput->Kind == InputReportKind::PointerUp) &&
			GetMouseCaptured() && forwardToCapturedControl())
		{
			UpdateCursor(mouse, contentMouse);
			break;
		}

		if (nativeInput->Kind == InputReportKind::MouseWheel
			|| nativeInput->Kind == InputReportKind::HorizontalMouseWheel)
		{
			const int delta = nativeInput->WheelDelta;
			Control* wheelHit = stagedHitControl;
			for (Control* target = wheelHit; target;
				target = target->GetRoutedParent())
			{
				if (!target->HandlesMouseWheel()) continue;
				int targetX = 0;
				int targetY = 0;
				if (!TryGetControlLocalPoint(
					target, contentMouse, targetX, targetY)) continue;
				if (!target->CanHandleMouseWheel(delta, targetX, targetY)) continue;
				if (cui::framework::InputAccess::DispatchInput(
					*target, nativeInput->Retarget(targetX, targetY)))
				{
					hitControl = target;
					break;
				}
			}
			if (!hitControl)
			{
				auto* target = stagedHitControl ? stagedHitControl : this;
				int targetX = 0;
				int targetY = 0;
				if (TryGetControlLocalPoint(
					target, contentMouse, targetX, targetY))
					(void)cui::framework::InputAccess::DispatchInput(
						*target,
						nativeInput->Retarget(targetX, targetY));
			}
			break;
		}

		auto* hit = stagedHitControl ? stagedHitControl : this;
		if (hit)
		{
			if (hit != this) hitControl = hit;
			int controlLocalX = 0;
			int controlLocalY = 0;
			if (!TryGetControlLocalPoint(
				hit, contentMouse, controlLocalX, controlLocalY))
			{
				hitControl = nullptr;
				break;
			}
			(void)cui::framework::InputAccess::DispatchInput(
				*hit,
				nativeInput->Retarget(controlLocalX, controlLocalY));
		}

		if (nativeInput->Kind == InputReportKind::PointerDown
			|| nativeInput->Kind == InputReportKind::PointerUp)
		{
			// PointerDown may transfer capture and therefore cursor ownership.
			// Re-resolve immediately instead of waiting for a later mouse move.
			UpdateCursor(mouse, contentMouse);
		}
	}
	break;
	case WM_CAPTURECHANGED:
	{
		if (nativeInput) (void)OnPreviewInputReport(*nativeInput);
		const bool captureMovedAway =
			reinterpret_cast<HWND>(lParam) != this->Handle;
		if (_inputManager && captureMovedAway)
			_inputManager->NotifyCaptureLost(*this);
	}
	break;
	case WM_CANCELMODE:
	{
		(void)DispatchInput(*nativeInput);
	}
	break;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	{
		(void)DispatchInput(*nativeInput);
	}
	break;
	case WM_SYSCHAR:
	{
		_lastKeyboardMessageHandled = ProcessAccessKey(static_cast<wchar_t>(wParam));
	}
	break;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
	case WM_KEYUP:
	case WM_SYSKEYUP:
	{
		(void)DispatchInput(*nativeInput);
	}
	break;
	case WM_SIZE:
	{
		RECT rec;
		GetClientRect(this->Handle, &rec);
		UINT width = (UINT)std::max<LONG>(1, rec.right - rec.left);
		UINT height = (UINT)std::max<LONG>(1, rec.bottom - rec.top);
		if (_renderHost)
		{
			_renderHost->Resize(width, height);
			_renderHost->SetDpi(_dpi);
		}
		SynchronizeNativeClientLayoutSlot();
		this->RequestLayout();
		this->Invalidate(false);
	}
	break;
	case WM_MOVE:
	{
		SynchronizeNativePosition();
		cui::framework::EventAccess::Raise(OnLocationChanged, this);
	}
	break;
	case WM_PAINT:
	{

	}
	break;
	};
	if (inputStaging && stagedMouseArgs)
		inputStaging->Complete(*stagedMouseArgs);
	if (_textCompositionManager
		&& (message == WM_KEYDOWN || message == WM_SYSKEYDOWN
			|| message == WM_LBUTTONDOWN || message == WM_LBUTTONUP))
		(void)_textCompositionManager->UpdateCaretPosition();
}
LRESULT CustomFrameHitTest(HWND _hWnd, WPARAM wParam, LPARAM lParam, int captionHeight, UINT dpi)
{
	const int scalerWidth = (std::max)(1,
		::MulDiv(8, static_cast<int>(dpi), 96));
	RECT wr, cr;
	const POINT ptMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	GetWindowRect(_hWnd, &wr);
	cr.left = wr.left + scalerWidth;
	cr.right = wr.right - scalerWidth;
	cr.bottom = wr.bottom - scalerWidth;
	cr.top = wr.top + scalerWidth;

	uint8_t pos_code = 0;
	if (ptMouse.x < wr.left || ptMouse.x > wr.right || ptMouse.y < wr.top || ptMouse.y > wr.bottom)
		return HTNOWHERE;

	if (ptMouse.x < cr.left)
		pos_code |= 0b01;
	else if (ptMouse.x > cr.right)
		pos_code |= 0b11;
	else
		pos_code |= 0b10;

	if (ptMouse.y < cr.top)
		pos_code |= 0b0100;
	else if (captionHeight > 0 && ptMouse.y < wr.top + captionHeight)
		return HTCAPTION;
	else if (ptMouse.y > cr.bottom)
		pos_code |= 0b1100;
	else
		pos_code |= 0b1000;

	switch (pos_code)
	{
	case 0b0101:
		return HTTOPLEFT;
	case 0b0110:
		return HTTOP;
	case 0b0111:
		return HTTOPRIGHT;
	case 0b1001:
		return HTLEFT;
	case 0b1010:
		return HTCLIENT;
	case 0b1011:
		return HTRIGHT;
	case 0b1101:
		return HTBOTTOMLEFT;
	case 0b1110:
		return HTBOTTOM;
	case 0b1111:
		return HTBOTTOMRIGHT;
	}
	return HTNOWHERE;
}
LRESULT Window::HandlePlatformWindowMessage(
	HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	Window* form = this;
	{
		const UINT animationDispatch = CuiAnimationFrameDispatchMessage();
		if (animationDispatch != 0 && message == animationDispatch)
		{
			auto frameLease = form->_animationFrameScheduler
				? form->_animationFrameScheduler->BeginDispatch(wParam)
				: AnimationFrameScheduler::DispatchLease{};
			if (!frameLease) return 0;
			const ControlWeakReference lifetime(form);
			(void)form->DispatchPendingAnimationInputSlice();
			form = dynamic_cast<Window*>(lifetime.Get());
			if (!form || form->Handle != hWnd
				|| ::IsWindow(hWnd) == FALSE) return 0;
			if (::IsWindowEnabled(hWnd) != FALSE)
			{
				form->InvalidateAnimatedControls(false);
				if (::IsWindowVisible(hWnd) != FALSE
					&& form->HasPendingRenderWork())
					::UpdateWindow(hWnd);
			}
			else
			{
				form->RefreshAnimationTimer();
			}
			return 0;
		}
		const UINT presentationDispatch = CuiPresentationDispatchMessage();
		if (presentationDispatch != 0 && message == presentationDispatch)
		{
			const bool isSettlementRetry = wParam == 1u;
			// A nested modal loop or a layout transaction may dispatch this turn.
			// Leave retained damage intact; EndWindowLayoutDeferral/WM_ENABLE will
			// schedule the next eligible presentation.
			if (form->_layoutDeferral.IsSuspended()
				|| ::IsWindowVisible(hWnd) == FALSE
				|| ::IsWindowEnabled(hWnd) == FALSE)
			{
				form->_layoutDispatchPosted = false;
				return 0;
			}
			// Layout and preparation can queue damage after WM_PAINT consumes its
			// entry damage. Settle one residual frame without monopolizing the UI
			// thread; failed or non-progressing frames yield to one posted retry.
			constexpr size_t maxSettlementPasses = 2u;
			size_t settlementPasses = 0u;
			const ControlWeakReference lifetime(form);
			while (settlementPasses < maxSettlementPasses
				&& form->HasPendingRenderWork())
			{
				RECT nativeUpdate{};
				if (::GetUpdateRect(hWnd, &nativeUpdate, FALSE) == FALSE)
					(void)::InvalidateRect(hWnd, nullptr, FALSE);
				const auto committedBefore =
					form->GetPresentationCommittedFrameCount();
				const BOOL updated = ::UpdateWindow(hWnd);
				++settlementPasses;
				form = dynamic_cast<Window*>(lifetime.Get());
				if (!form || form->Handle != hWnd
					|| ::IsWindow(hWnd) == FALSE) return 0;
				const auto committedAfter =
					form->GetPresentationCommittedFrameCount();
				if (updated == FALSE || committedAfter <= committedBefore) break;
			}
			form->_layoutDispatchPosted = false;
			bool retryPosted = false;
			if (!isSettlementRetry && form->HasPendingRenderWork()
				&& !form->_layoutDeferral.IsSuspended()
				&& ::IsWindowVisible(hWnd) != FALSE
				&& ::IsWindowEnabled(hWnd) != FALSE)
			{
				// Yield once before the final bounded attempt.
				retryPosted = ::PostMessageW(
					hWnd, presentationDispatch, 1u, 0u) != FALSE;
				form->_layoutDispatchPosted = retryPosted;
			}
			// Park the native region after the bounded retry, but retain logical
			// damage for the next external invalidation epoch.
			const bool hasRetainedDamage = form->_renderHost
				&& (form->_presentationInvalidated
					|| form->_renderHost->HasPendingDamage()
					|| form->_renderHost->NeedsFullFrame());
			if (form->HasPendingRenderWork() && hasRetainedDamage
				&& (isSettlementRetry || !retryPosted))
				(void)::ValidateRect(hWnd, nullptr);
			return 0;
		}
		if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED
			|| message == WM_SYSCOLORCHANGE)
		{
			form->RefreshSystemVisualPreferences();
			form->Invalidate(true);
		}
		if (message == WM_DPICHANGED)
		{
			UINT newDpi = HIWORD(wParam);
			RECT* suggested = (RECT*)lParam;
			if (suggested)
			{
				SetWindowPos(hWnd, nullptr,
					suggested->left,
					suggested->top,
					suggested->right - suggested->left,
					suggested->bottom - suggested->top,
					SWP_NOZORDER | SWP_NOACTIVATE);
			}
			// 尺寸/DPI 变化后，强制同步渲染目标尺寸并安排一次重绘，避免出现新区域未刷新。
			form->SyncRenderSizeToClient();
			if (form->_renderHost)
				form->_renderHost->InvalidateFrameHistory();
			form->Invalidate(false);
			// 若窗口尚未首次显示，控件树可能还未构造完成：此时只记录 DPI，真正缩放留到 Show 前。
			if (!form->_initialDpiApplied)
			{
				form->_dpi = newDpi;
				return 0;
			}
			form->ApplyDpiChange(newDpi);
			return 0;
		}
		if (message == WM_GETOBJECT)
		{
			const LRESULT accessible = form->HandleAccessibleObjectRequest(wParam, lParam);
			if (accessible != 0) return accessible;
		}

		if (message == WM_CLOSE)
		{
			if (form->_closingEventActive) return 0;
			form->_closingEventActive = true;
			const ControlWeakReference lifetime(form);
			CancelEventArgs args;
			bool handlerFailed = false;
			try
			{
				cui::framework::EventAccess::Raise(
					form->OnClosing, form, args);
			}
			catch (...)
			{
				// Exceptions must not escape a native window procedure or leave
				// Closing permanently latched. During ordinary Close a failing
				// handler cancels; Application shutdown, like WPF, cannot be
				// canceled by Closing.
				if (auto* live = dynamic_cast<Window*>(lifetime.Get()))
					live->_closingEventActive = false;
				handlerFailed = true;
			}
			auto* live = static_cast<Window*>(lifetime.Get());
			if (!live) return 0;
			live->_closingEventActive = false;
			const bool applicationShutdown =
				Application::IsWindowClosingForShutdown(*live);
			if (((args.Cancel || handlerFailed) && !applicationShutdown)
				|| !::IsWindow(hWnd)) return 0;
			// Window owns its lifecycle messages.  Native hooks cannot consume an
			// already accepted close after the cancellable Closing event ran.
			return ::DefWindowProcW(hWnd, message, wParam, lParam);
		}
		if (auto handled = form->OnPlatformMessage(message, wParam, lParam))
			return *handled;
		form->ProcessPlatformMessage(message, wParam, lParam);
		if (form->_hasDirectMessageResult)
			return form->_directMessageResult;
		if (form->_lastKeyboardMessageHandled)
			return 0;

		switch (message)
		{
		case WM_NCCALCSIZE:
		{
			if (wParam)
			{
				NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
				if (params)
				{
					if (IsZoomed(hWnd))
					{
						const int inset = GetCustomFrameInset();
						params->rgrc[0].left += inset;
						params->rgrc[0].right -= inset;
						params->rgrc[0].top += inset;
						params->rgrc[0].bottom -= inset;
					}
					return 0;
				}
			}
			break;
		}
		case WM_SETCURSOR:
		{
			if (LOWORD(lParam) == HTCLIENT)
			{
				form->UpdateCursorFromCurrentMouse();
				return TRUE;
			}
		}
		break;
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT:
		{
			// Native modal loops disable their owner before dispatching queued
			// owner messages.  Presenting a swap chain from that nested loop can
			// block behind the modal surface and prevent the dialog from ever
			// becoming visible.  Validate only the HWND update region here and
			// retain presentation damage for the first enabled frame.
			if (::IsWindowEnabled(hWnd) == FALSE)
			{
				PAINTSTRUCT ps{};
				::BeginPaint(hWnd, &ps);
				::EndPaint(hWnd, &ps);
				return 0;
			}
			RECT pendingPaint{};
			const bool hadPendingPaint =
				::GetUpdateRect(hWnd, &pendingPaint, FALSE) != FALSE;
			RECT queuedDamage{};
			const bool hadQueuedDamage = form->_renderHost
				&& form->_renderHost->TakePendingDamage(queuedDamage);
			PAINTSTRUCT ps{};
			BeginPaint(hWnd, &ps);
			RECT paintDirty = ps.rcPaint;
			if (paintDirty.right <= paintDirty.left
				|| paintDirty.bottom <= paintDirty.top)
			{
				if (hadPendingPaint)
					paintDirty = pendingPaint;
				else if (hadQueuedDamage)
					paintDirty = queuedDamage;
				else if (form->_presentationInvalidated
					|| (form->_renderHost
						&& form->_renderHost->NeedsFullFrame()))
					::GetClientRect(hWnd, &paintDirty);
			}
			if (hadQueuedDamage)
			{
				if (paintDirty.right > paintDirty.left
					&& paintDirty.bottom > paintDirty.top)
				{
					RECT combined{};
					::UnionRect(&combined, &paintDirty, &queuedDamage);
					paintDirty = combined;
				}
				else
				{
					paintDirty = queuedDamage;
				}
			}
			const bool hasPaintDirty = paintDirty.right > paintDirty.left
				&& paintDirty.bottom > paintDirty.top;
			bool rendered = false;
			if (form->_renderHost && hasPaintDirty
				&& (form->_renderHost->IsAttached()
					|| form->_renderHost->IsDeviceLost()))
			{
				// A transient initial HwndGraphics construction failure keeps the
				// HWND plus a reset request but intentionally has no PrimaryContext.
				// UpdateDirtyRect owns recovery, so the paint path must remain
				// reachable before IsAttached becomes true.
				if (form->_presentationInvalidated
					|| form->_renderHost->NeedsFullFrame()
					|| hadPendingPaint || hadQueuedDamage)
					rendered = form->UpdateDirtyRect(paintDirty, false);
			}
			if (!rendered && hasPaintDirty && form->_renderHost)
			{
				// Preserve logical damage without recreating the update region from
				// inside BeginPaint/EndPaint and spinning WM_PAINT.
				form->_presentationInvalidated = true;
			}
			EndPaint(hWnd, &ps);
			// Native exposure can enter WM_PAINT without a presentation token.
			// Promote retained work after EndPaint validates the update region.
			if (form->HasPendingRenderWork()
				&& !form->_layoutDispatchPosted)
				form->ScheduleLayoutDispatch();
			return 0;
		}
		case WM_ENABLE:
		{
			if (wParam != FALSE)
			{
				// WM_PAINT intentionally leaves retained damage untouched while
				// disabled.  Promote the resumed frame to a complete repaint so
				// native modal occlusion never exposes a stale back buffer.
				form->Invalidate(false);
			}
			form->RefreshAnimationTimer();
			return 0;
		}
		case WM_TIMER:
		{
			if (wParam == form->_animTimerId
				&& form->_animationUsesLegacyTimer)
			{
				if (::IsWindowEnabled(hWnd) != FALSE)
					form->InvalidateAnimatedControls(false);
				return 0;
			}
		}
		break;
		case WM_ACTIVATE:
		{
			constexpr MARGINS margins{ 1, 1, 1, 1 };
			DwmExtendFrameIntoClientArea(hWnd, &margins);
		}
		break;
		case WM_NCHITTEST:
		{
			LRESULT hitTestResult;
			if (!DwmDefWindowProc(hWnd, message, wParam, lParam, &hitTestResult))
			{
				POINT ptClient{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				ScreenToClient(hWnd, &ptClient);
				if (form->HitTestCaptionButtonResizeExclusion(ptClient))
					return HTCLIENT;

				hitTestResult = CustomFrameHitTest(
					hWnd, wParam, lParam,
					form->GetTitleBarHeightPixels(), form->_dpi);
				if (hitTestResult == HTCAPTION)
				{
					CaptionButtonKind k{};
					if (form->HitTestCaptionButtons(ptClient, k))
						return HTCLIENT;
				}
				if (IsZoomed(hWnd))
				{
					// 最大化状态下禁止鼠标拖拽边缘/角落调整窗口大小
					if (hitTestResult == HTLEFT || hitTestResult == HTRIGHT || hitTestResult == HTTOP || hitTestResult == HTBOTTOM ||
						hitTestResult == HTTOPLEFT || hitTestResult == HTTOPRIGHT || hitTestResult == HTBOTTOMLEFT || hitTestResult == HTBOTTOMRIGHT)
					{
						return HTCLIENT;
					}
				}
				if (!form->CanResizeWindow())
				{
					// 禁用边缘/角落 resize，只保留标题栏拖动与正常客户区
					if (hitTestResult == HTLEFT || hitTestResult == HTRIGHT || hitTestResult == HTTOP || hitTestResult == HTBOTTOM ||
						hitTestResult == HTTOPLEFT || hitTestResult == HTTOPRIGHT || hitTestResult == HTBOTTOMLEFT || hitTestResult == HTBOTTOMRIGHT)
					{
						return HTCLIENT;
					}
				}
				if (hitTestResult != HTCAPTION)
				{
					return hitTestResult;
				}
			}
		}
		break;
		case WM_NCDESTROY:
		{
			cui::framework::EventAccess::Raise(form->OnWindowClosed, form);
			Application::UnregisterWindow(*form);
			form->CleanupResources();
		}
		break;
	}
	}
	return DefWindowProc(hWnd, message, wParam, lParam);
}
