/*
	SDbgExt2 - Copyright (C) 2013, Steve Niemitz

    SDbgExt2 is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    SDbgExt2 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with SDbgExt2.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"
#include "WinDbgExt.h"
#include <metahost.h>
#include <vector>
#include <sstream>

BOOL g_ClrLoaded = FALSE;
CComPtr<ICLRRuntimeHost> g_ClrHost;

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

HRESULT InitClr()
{
	CComPtr<ICLRMetaHost> metaHost;
	CComPtr<ICLRRuntimeInfo> runtime;
	HRESULT hr = S_OK;
	RETURN_IF_FAILED(CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (LPVOID*)&metaHost));
	RETURN_IF_FAILED(metaHost->GetRuntime(L"v4.0.30319", IID_ICLRRuntimeInfo, (LPVOID*)&runtime));
	RETURN_IF_FAILED(runtime->GetInterface(CLSID_CLRRuntimeHost, IID_ICLRRuntimeHost, (LPVOID*)&g_ClrHost));

	g_ClrLoaded = TRUE;

	return g_ClrHost->Start();
}

class DbgHelper :
	public IDbgHelper,
	public CComObjectRoot
{
	BEGIN_COM_MAP(DbgHelper)
		COM_INTERFACE_ENTRY(IDbgHelper)
	END_COM_MAP()

public:
	static HRESULT CreateInstance(CComPtr<IDebugControl4> ctrl, IDbgHelper **ret)
	{
		CComObject<DbgHelper> *co;
		CComObject<DbgHelper>::CreateInstance(&co);
		co->AddRef();
		co->m_ctrl = ctrl;

		*ret = co;
		return S_OK;
	}

	STDMETHODIMP Output(ULONG mask, LPWSTR text)
	{
		return m_ctrl->OutputWide(mask, L"%s", text);
	}

	STDMETHODIMP GetWinDbgExpression(LPSTR expr, CLRDATA_ADDRESS *addr)
	{
		*addr = GetExpression(expr);
		return S_OK;
	}

	STDMETHODIMP GetThreads(ULONG iThreadSize, ULONG threads[], DWORD *numThreads)
	{
		UNREFERENCED_PARAMETER(iThreadSize);
		UNREFERENCED_PARAMETER(threads);
		UNREFERENCED_PARAMETER(numThreads);

		CComQIPtr<IDebugSystemObjects> dso(m_ctrl);

		if (SUCCEEDED(dso->GetNumberThreads(numThreads)))
		{
			std::vector<ULONG> osIds(*numThreads);
			std::vector<ULONG> sysIds(*numThreads);
			if (SUCCEEDED(dso->GetThreadIdsByIndex(0, *numThreads, osIds.data(), sysIds.data())))
			{
				std::copy_n(sysIds.begin(), min(sysIds.size(), iThreadSize), stdext::checked_array_iterator<ULONG*>(threads, iThreadSize));
			}
		}

		return S_FALSE;
	}

private:
	CComPtr<IDebugControl4> m_ctrl;

};

HRESULT ExecuteOpCode(WinDbgInterfaces *dbg, UINT opcode, PCSTR subOpCode, PCSTR args)
{
	CoInitialize(NULL);

	HRESULT hr;
	if (!g_ClrLoaded)
	{
		if (FAILED(hr = InitClr()))
		{
			dwdprintf(dbg->Control, L"Unable to initialize the CLR, return code : 0x%08x\r\n", hr);
			return S_OK;
		}
	}

	DWORD returnValue = 0;

	// This needs to stay in sync w/ the .NET version
	struct InterfaceBuffer
	{
		CComPtr<IXCLRDataProcess3> XCLR;
		CComPtr<IClrProcess> Process;
		CComPtr<ISDbgExt> Ext;
		CComPtr<IDbgHelper> Helper;
	};

	CComPtr<IDbgHelper> helper;
	DbgHelper::CreateInstance(dbg->Control, &helper);

	InterfaceBuffer ib = { dbg->XCLR, dbg->Process, dbg->Ext, helper };	
	std::wstringstream wss;

	// Copy the address of the extension object into a string.  
	// Ref counting isn't a big deal here, we can guarantee that InitHost will AddRef the objects before it returns. 
	// This will be then marshalled into .NET memory inside of InitHost
	wss << opcode << L"|" << (ULONG64)(void*)(&ib) << L"|";

	if (subOpCode != nullptr)
	{
		wss << subOpCode << L"|";
	}
	if (args != nullptr && strlen(args) > 0)
	{
		wss << args;
	}

	WCHAR dllPathBuffer[MAX_PATH];
	GetModuleFileNameW((HINSTANCE)&__ImageBase, dllPathBuffer, ARRAYSIZE(dllPathBuffer));

	std::wstring dllPath(dllPathBuffer);
	size_t lastSlash = dllPath.rfind('\\');
	dllPath = dllPath.substr(0, lastSlash);
	dllPath += L"\\SPTM.dll";

	hr = g_ClrHost->ExecuteInDefaultAppDomain(dllPath.c_str(), L"SPT.Managed.UMThunk", L"InitHost", wss.str().c_str(), &returnValue);
	if (FAILED(hr))
	{
		dwdprintf(dbg->Control, L"Unable to load SPTM.dll, please make sure it's in the same directory as SPT.dll");
	}

	if (returnValue != 1)
	{
		return E_FAIL;
	}

	return hr;
}

DBG_FUNC(scriptit)
{
	DBG_PREAMBLE;
	UNREFERENCED_PARAMETER(hr);

	return ExecuteOpCode(&dbg, 0, nullptr, args);
}