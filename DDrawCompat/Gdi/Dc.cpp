#include <algorithm>
#include <unordered_map>

#include "Common/Hook.h"
#include "Common/Log.h"
#include "Common/ScopedCriticalSection.h"
#include "Gdi/Dc.h"
#include "Gdi/DcCache.h"
#include "Gdi/Gdi.h"
#include "Gdi/Window.h"

namespace
{
	using Gdi::DcCache::CachedDc;

	struct CompatDc : CachedDc
	{
		CompatDc(const CachedDc& cachedDc = {}) : CachedDc(cachedDc) {}
		DWORD refCount;
		HDC origDc;
		int savedState;
	};

	typedef std::unordered_map<HDC, CompatDc> CompatDcMap;
	CompatDcMap g_origDcToCompatDc;

	void copyDcAttributes(CompatDc& compatDc, HDC origDc, POINT& origin)
	{
		SelectObject(compatDc.dc, GetCurrentObject(origDc, OBJ_FONT));
		SelectObject(compatDc.dc, GetCurrentObject(origDc, OBJ_BRUSH));
		SelectObject(compatDc.dc, GetCurrentObject(origDc, OBJ_PEN));

		if (GM_ADVANCED == GetGraphicsMode(origDc))
		{
			SetGraphicsMode(compatDc.dc, GM_ADVANCED);
			XFORM transform = {};
			GetWorldTransform(origDc, &transform);
			SetWorldTransform(compatDc.dc, &transform);
		}

		SetMapMode(compatDc.dc, GetMapMode(origDc));

		POINT viewportOrg = {};
		GetViewportOrgEx(origDc, &viewportOrg);
		SetViewportOrgEx(compatDc.dc, viewportOrg.x + origin.x, viewportOrg.y + origin.y, nullptr);
		SIZE viewportExt = {};
		GetViewportExtEx(origDc, &viewportExt);
		SetViewportExtEx(compatDc.dc, viewportExt.cx, viewportExt.cy, nullptr);

		POINT windowOrg = {};
		GetWindowOrgEx(origDc, &windowOrg);
		SetWindowOrgEx(compatDc.dc, windowOrg.x, windowOrg.y, nullptr);
		SIZE windowExt = {};
		GetWindowExtEx(origDc, &windowExt);
		SetWindowExtEx(compatDc.dc, windowExt.cx, windowExt.cy, nullptr);

		SetArcDirection(compatDc.dc, GetArcDirection(origDc));
		SetBkColor(compatDc.dc, GetBkColor(origDc));
		SetBkMode(compatDc.dc, GetBkMode(origDc));
		SetDCBrushColor(compatDc.dc, GetDCBrushColor(origDc));
		SetDCPenColor(compatDc.dc, GetDCPenColor(origDc));
		SetLayout(compatDc.dc, GetLayout(origDc));
		SetPolyFillMode(compatDc.dc, GetPolyFillMode(origDc));
		SetROP2(compatDc.dc, GetROP2(origDc));
		SetStretchBltMode(compatDc.dc, GetStretchBltMode(origDc));
		SetTextAlign(compatDc.dc, GetTextAlign(origDc));
		SetTextCharacterExtra(compatDc.dc, GetTextCharacterExtra(origDc));
		SetTextColor(compatDc.dc, GetTextColor(origDc));

		POINT brushOrg = {};
		GetBrushOrgEx(origDc, &brushOrg);
		SetBrushOrgEx(compatDc.dc, brushOrg.x, brushOrg.y, nullptr);

		POINT currentPos = {};
		GetCurrentPositionEx(origDc, &currentPos);
		MoveToEx(compatDc.dc, currentPos.x, currentPos.y, nullptr);
	}

	void setClippingRegion(HDC compatDc, HDC origDc, HWND hwnd, const POINT& origin)
	{
		if (hwnd)
		{
			HRGN sysRgn = CreateRectRgn(0, 0, 0, 0);
			if (1 == GetRandomRgn(origDc, sysRgn, SYSRGN))
			{
				SelectClipRgn(compatDc, sysRgn);
				SetMetaRgn(compatDc);
			}
			DeleteObject(sysRgn);
		}

		HRGN clipRgn = CreateRectRgn(0, 0, 0, 0);
		if (1 == GetClipRgn(origDc, clipRgn))
		{
			OffsetRgn(clipRgn, origin.x, origin.y);
			SelectClipRgn(compatDc, clipRgn);
		}
		DeleteObject(clipRgn);
	}

	void updateWindow(HWND wnd)
	{
		RECT windowRect = {};
		GetWindowRect(wnd, &windowRect);

		auto& window = Gdi::Window::get(wnd);
		RECT cachedWindowRect = window.getWindowRect();

		if (!EqualRect(&windowRect, &cachedWindowRect))
		{
			Gdi::Window::updateAll();
		}
	}
}

namespace Gdi
{
	namespace Dc
	{
		HDC getDc(HDC origDc)
		{
			if (!origDc || OBJ_DC != GetObjectType(origDc) || DT_RASDISPLAY != GetDeviceCaps(origDc, TECHNOLOGY))
			{
				return nullptr;
			}

			Compat::ScopedCriticalSection gdiLock(Gdi::g_gdiCriticalSection);

			auto it = g_origDcToCompatDc.find(origDc);
			if (it != g_origDcToCompatDc.end())
			{
				++it->second.refCount;
				return it->second.dc;
			}

			const HWND wnd = CALL_ORIG_FUNC(WindowFromDC)(origDc);
			const HWND rootWnd = wnd ? GetAncestor(wnd, GA_ROOT) : nullptr;
			if (rootWnd && GetDesktopWindow() != rootWnd)
			{
				updateWindow(rootWnd);
			}

			CompatDc compatDc(Gdi::DcCache::getDc());
			if (!compatDc.dc)
			{
				return nullptr;
			}

			POINT origin = {};
			GetDCOrgEx(origDc, &origin);

			compatDc.savedState = SaveDC(compatDc.dc);
			copyDcAttributes(compatDc, origDc, origin);
			setClippingRegion(compatDc.dc, origDc, wnd, origin);

			compatDc.refCount = 1;
			compatDc.origDc = origDc;
			g_origDcToCompatDc.insert(CompatDcMap::value_type(origDc, compatDc));

			return compatDc.dc;
		}

		HDC getOrigDc(HDC dc)
		{
			const auto it = std::find_if(g_origDcToCompatDc.begin(), g_origDcToCompatDc.end(),
				[dc](const CompatDcMap::value_type& compatDc) { return compatDc.second.dc == dc; });
			return it != g_origDcToCompatDc.end() ? it->first : dc;
		}

		void releaseDc(HDC origDc)
		{
			Compat::ScopedCriticalSection gdiLock(Gdi::g_gdiCriticalSection);

			auto it = g_origDcToCompatDc.find(origDc);
			if (it == g_origDcToCompatDc.end())
			{
				return;
			}

			CompatDc& compatDc = it->second;
			--compatDc.refCount;
			if (0 == compatDc.refCount)
			{
				RestoreDC(compatDc.dc, compatDc.savedState);
				Gdi::DcCache::releaseDc(compatDc);
				g_origDcToCompatDc.erase(origDc);
			}
		}
	}
}
