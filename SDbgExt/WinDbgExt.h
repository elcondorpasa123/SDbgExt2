#pragma once

#include "SDbgExtApi.h"
#define KDEXT_64BIT
#include <DbgEng.h>
#include <WDBGEXTS.H>
#include "SR.h"

#define DBG_API extern "C" __declspec(dllexport) HRESULT
#define DBG_FUNC(name) DBG_API name(PDEBUG_CLIENT clientPtr, PCSTR args)
#define dwdprintf(ctrl, x,...) dbg.Control->ControlledOutputWide(DEBUG_OUTCTL_AMBIENT_DML, DEBUG_OUTPUT_NORMAL, ##x, __VA_ARGS__)
//#define dwdprintf_res(ctrl, resName, ...) dwdprintf(ctrl, SR::resName(), __VA_ARGS__);

HRESULT CreateClrProcessFromWinDbg(CComPtr<IDebugClient> client, IClrProcess **proc);

#define DBG_PREAMBLE	HRESULT hr = S_OK; \
						CComPtr<IDebugClient> client(clientPtr); \
						WinDbgInterfaces dbg(client); \
											
extern PDEBUG_CLIENT g_Client;

struct WinDbgInterfaces
{
	WinDbgInterfaces(CComPtr<IDebugClient> client)
	{
		CreateClrProcessFromWinDbg(client, &Process);
		CreateSDbgExt(Process, &Ext);

		Client = client;
		client.QueryInterface<IDebugControl4>(&Control);
	}

	CComPtr<IClrProcess> Process;
	CComPtr<ISDbgExt> Ext;

	CComPtr<IDebugClient> Client;
	CComPtr<IDebugControl4> Control;
};