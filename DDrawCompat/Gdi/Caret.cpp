#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#include "Common/Hook.h"
#include "Common/ScopedCriticalSection.h"
#include "Gdi/AccessGuard.h"
#include "Gdi/Caret.h"
#include "Gdi/Dc.h"
#include "Gdi/Gdi.h"

namespace
{
	HWINEVENTHOOK g_compatGdiCaretGeneralEventHook = nullptr;
	HWINEVENTHOOK g_compatGdiCaretLocationChangeEventHook = nullptr;

	template <typename Result, typename... Params>
	using FuncPtr = Result(WINAPI *)(Params...);

	struct CaretData
	{
		HWND hwnd;
		long left;
		long top;
		long width;
		long height;
		bool isVisible;

		bool operator==(const CaretData& rhs) const
		{
			return hwnd == rhs.hwnd &&
				left == rhs.left &&
				top == rhs.top &&
				width == rhs.width &&
				height == rhs.height &&
				isVisible == rhs.isVisible;
		}
	};


	CaretData g_caret = {};
	CRITICAL_SECTION g_caretCriticalSection;
	
	void updateCaret();

	void CALLBACK compatGdiCaretEvent(HWINEVENTHOOK, DWORD, HWND, LONG idObject, LONG, DWORD, DWORD)
	{
		if (OBJID_CARET == idObject)
		{
			updateCaret();
		}
	}

	template <typename OrigFuncPtr, OrigFuncPtr origFunc, typename Result, typename... Params>
	Result WINAPI compatGdiCaretFunc(Params... params)
	{
		Result result = Compat::getOrigFuncPtr<OrigFuncPtr, origFunc>()(params...);
		updateCaret();
		return result;
	}

	void drawCaret(const CaretData& caret)
	{
		if (caret.isVisible)
		{
			HDC dc = GetDC(caret.hwnd);
			HDC compatDc = Gdi::Dc::getDc(dc);
			CALL_ORIG_FUNC(PatBlt)(
				compatDc, caret.left, caret.top, caret.width, caret.height, PATINVERT);
			Gdi::Dc::releaseDc(dc);
			ReleaseDC(caret.hwnd, dc);
		}
	}

	CaretData getCaretData()
	{
		GUITHREADINFO gti = {};
		gti.cbSize = sizeof(gti);
		GetGUIThreadInfo(Gdi::getGdiThreadId(), &gti);

		CaretData caretData = {};
		caretData.hwnd = gti.hwndCaret;
		caretData.left = gti.rcCaret.left;
		caretData.top = gti.rcCaret.top;
		caretData.width = gti.rcCaret.right - gti.rcCaret.left;
		caretData.height = gti.rcCaret.bottom - gti.rcCaret.top;
		caretData.isVisible = gti.flags & GUI_CARETBLINKING;
		return caretData;
	}

	template <typename OrigFuncPtr, OrigFuncPtr origFunc, typename Result, typename... Params>
	OrigFuncPtr getCompatGdiCaretFuncPtr(FuncPtr<Result, Params...>)
	{
		return &compatGdiCaretFunc<OrigFuncPtr, origFunc, Result, Params...>;
	}

	void updateCaret()
	{
		Compat::ScopedCriticalSection lock(g_caretCriticalSection);

		CaretData newCaret = getCaretData();
		if (newCaret == g_caret)
		{
			return;
		}

		if ((g_caret.isVisible || newCaret.isVisible))
		{
			Gdi::GdiAccessGuard accessGuard(Gdi::ACCESS_WRITE);
			drawCaret(g_caret);
			drawCaret(newCaret);
		}

		g_caret = newCaret;
	}
}

#define HOOK_GDI_CARET_FUNCTION(module, func) \
	Compat::hookFunction<decltype(&func), &func>( \
		#module, #func, getCompatGdiCaretFuncPtr<decltype(&func), &func>(&func));

namespace Gdi
{
	namespace Caret
	{
		void installHooks()
		{
			InitializeCriticalSection(&g_caretCriticalSection);

			HOOK_GDI_CARET_FUNCTION(user32, CreateCaret);
			HOOK_GDI_CARET_FUNCTION(user32, DestroyCaret);
			HOOK_GDI_CARET_FUNCTION(user32, HideCaret);
			HOOK_GDI_CARET_FUNCTION(user32, SetCaretPos);
			HOOK_GDI_CARET_FUNCTION(user32, ShowCaret);

			const DWORD threadId = Gdi::getGdiThreadId();
			g_compatGdiCaretGeneralEventHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE,
				nullptr, &compatGdiCaretEvent, 0, threadId, WINEVENT_OUTOFCONTEXT);
			g_compatGdiCaretLocationChangeEventHook = SetWinEventHook(
				EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, &compatGdiCaretEvent,
				0, threadId, WINEVENT_OUTOFCONTEXT);
		}

		void uninstallHooks()
		{
			UnhookWinEvent(g_compatGdiCaretLocationChangeEventHook);
			UnhookWinEvent(g_compatGdiCaretGeneralEventHook);
		}
	}
}
