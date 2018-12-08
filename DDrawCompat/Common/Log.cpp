#define WIN32_LEAN_AND_MEAN

#include <vector>

#include <Windows.h>

#include "Common/Log.h"

namespace
{
	template <typename DevMode>
	std::ostream& streamDevMode(std::ostream& os, const DevMode& dm)
	{
		return Compat::LogStruct(os)
			<< dm.dmPelsWidth
			<< dm.dmPelsHeight
			<< dm.dmBitsPerPel
			<< dm.dmDisplayFrequency
			<< dm.dmDisplayFlags;
	}
}

std::ostream& operator<<(std::ostream& os, const char* str)
{
	if (!str)
	{
		return os << "null";
	}

	if (!Compat::Log::isPointerDereferencingAllowed() || reinterpret_cast<DWORD>(str) <= 0xFFFF)
	{
		return os << static_cast<const void*>(str);
	}

	return os.write(str, strlen(str));
}

std::ostream& operator<<(std::ostream& os, const unsigned char* data)
{
	return os << static_cast<const void*>(data);
}

std::ostream& operator<<(std::ostream& os, const WCHAR* wstr)
{
	if (!wstr)
	{
		return os << "null";
	}

	if (!Compat::Log::isPointerDereferencingAllowed() || reinterpret_cast<DWORD>(wstr) <= 0xFFFF)
	{
		return os << static_cast<const void*>(wstr);
	}

	return os << std::string(wstr, wstr + wcslen(wstr)).c_str();
}

std::ostream& operator<<(std::ostream& os, const DEVMODEA& dm)
{
	return streamDevMode(os, dm);
}

std::ostream& operator<<(std::ostream& os, const DEVMODEW& dm)
{
	return streamDevMode(os, dm);
}

std::ostream& operator<<(std::ostream& os, const RECT& rect)
{
	return Compat::LogStruct(os)
		<< rect.left
		<< rect.top
		<< rect.right
		<< rect.bottom;
}

std::ostream& operator<<(std::ostream& os, HDC__& dc)
{
	return os << "DC(" << static_cast<void*>(&dc) << ',' << WindowFromDC(&dc) << ')';
}

std::ostream& operator<<(std::ostream& os, HRGN__& rgn)
{
	DWORD size = GetRegionData(&rgn, 0, nullptr);
	if (0 == size)
	{
		return os << "RGN[]";
	}

	std::vector<unsigned char> rgnDataBuf(size);
	auto& rgnData = *reinterpret_cast<RGNDATA*>(rgnDataBuf.data());
	GetRegionData(&rgn, size, &rgnData);

	return os << "RGN" << Compat::array(reinterpret_cast<RECT*>(rgnData.Buffer), rgnData.rdh.nCount);
}

std::ostream& operator<<(std::ostream& os, HWND__& hwnd)
{
	char name[256] = "INVALID";
	RECT rect = {};
	if (IsWindow(&hwnd))
	{
		GetClassName(&hwnd, name, sizeof(name));
		GetWindowRect(&hwnd, &rect);
	}
	return os << "WND(" << static_cast<void*>(&hwnd) << ',' << name << ',' << rect << ')';
}

std::ostream& operator<<(std::ostream& os, const DDSCAPS& caps)
{
	return Compat::LogStruct(os)
		<< Compat::hex(caps.dwCaps);
}

std::ostream& operator<<(std::ostream& os, const DDSCAPS2& caps)
{
	return Compat::LogStruct(os)
		<< Compat::hex(caps.dwCaps)
		<< Compat::hex(caps.dwCaps2)
		<< Compat::hex(caps.dwCaps3)
		<< Compat::hex(caps.dwCaps4);
}

std::ostream& operator<<(std::ostream& os, const DDPIXELFORMAT& pf)
{
	return Compat::LogStruct(os)
		<< Compat::hex(pf.dwFlags)
		<< Compat::hex(pf.dwFourCC)
		<< pf.dwRGBBitCount
		<< Compat::hex(pf.dwRBitMask)
		<< Compat::hex(pf.dwGBitMask)
		<< Compat::hex(pf.dwBBitMask)
		<< Compat::hex(pf.dwRGBAlphaBitMask);
}

std::ostream& operator<<(std::ostream& os, const DDSURFACEDESC& sd)
{
	DDSURFACEDESC2 sd2 = {};
	memcpy(&sd2, &sd, sizeof(sd));
	return os << sd2;
}

std::ostream& operator<<(std::ostream& os, const DDSURFACEDESC2& sd)
{
	return Compat::LogStruct(os)
		<< Compat::hex(sd.dwFlags)
		<< sd.dwHeight
		<< sd.dwWidth
		<< sd.lPitch
		<< sd.dwBackBufferCount
		<< sd.dwMipMapCount
		<< sd.dwAlphaBitDepth
		<< sd.dwReserved
		<< sd.lpSurface
		<< sd.ddpfPixelFormat
		<< sd.ddsCaps
		<< sd.dwTextureStage;
}

std::ostream& operator<<(std::ostream& os, const CWPSTRUCT& cwrp)
{
	return Compat::LogStruct(os)
		<< Compat::hex(cwrp.message)
		<< cwrp.hwnd
		<< Compat::hex(cwrp.wParam)
		<< Compat::hex(cwrp.lParam);
}

std::ostream& operator<<(std::ostream& os, const CWPRETSTRUCT& cwrp)
{
	return Compat::LogStruct(os)
		<< Compat::hex(cwrp.message)
		<< cwrp.hwnd
		<< Compat::hex(cwrp.wParam)
		<< Compat::hex(cwrp.lParam)
		<< Compat::hex(cwrp.lResult);
}

namespace Compat
{
	Log::Log()
	{
		SYSTEMTIME st = {};
		GetLocalTime(&st);

		char time[100];
		sprintf_s(time, "%02hu:%02hu:%02hu.%03hu ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

		s_logFile << GetCurrentThreadId() << " " << time;
	}

	Log::~Log()
	{
		s_logFile << std::endl;
	}

	std::ofstream Log::s_logFile("ddraw.log");
	DWORD Log::s_outParamDepth = 0;
	bool Log::s_isLeaveLog = false;
}
