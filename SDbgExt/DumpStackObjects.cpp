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
#include "..\SDbgCore\inc\ClrObject.h"
#include <cor.h>
#include <algorithm>

struct Register
{
	Register(WCHAR *name, CLRDATA_ADDRESS value)
	: Name(name), Value(value)
	{}

	WCHAR *Name;
	CLRDATA_ADDRESS Value;
};

HRESULT GetArchRegisters(WinDbgInterfaces *dbg, std::vector<Register> *registers, CLRDATA_ADDRESS *framePointer)
{
	HRESULT hr = S_OK;
	CComPtr<IDebugAdvanced> adv;
	CONTEXT ctx = {};
	RETURN_IF_FAILED(dbg->Client->QueryInterface(__uuidof(IDebugAdvanced), (PVOID*)&adv));
	adv->GetThreadContext(&ctx, sizeof(CONTEXT));

#ifdef _M_X64
	*framePointer = ctx.Rsp;
#else
	*framePointer = ctx.Esp;
	registers->emplace_back(Register(L"eax", ctx.Eax));
	registers->emplace_back(Register(L"ebx", ctx.Ebx));
	registers->emplace_back(Register(L"ecx", ctx.Ecx));
	registers->emplace_back(Register(L"edx", ctx.Edx));
	registers->emplace_back(Register(L"esi", ctx.Esi));
	registers->emplace_back(Register(L"edi", ctx.Edi));
#endif

	return S_OK;
}

void HandleObject(WinDbgInterfaces *dbg, ClrObjectData o)
{
	dwdprintf(dbg->Control, L" %p", o.ObjectAddress);
	WCHAR buffer[512];
	UINT len;
	if (SUCCEEDED(dbg->XCLR->GetMethodTableName(o.MethodTable, ARRAYSIZE(buffer), buffer, &len)))
	{
		dwdprintf(dbg->Control, L" %s", buffer);
	}
	if (o.ObjectType == OBJ_STRING
		&& SUCCEEDED(dbg->XCLR->GetObjectStringData(o.ObjectAddress, ARRAYSIZE(buffer), buffer, &len)))
	{
		dwdprintf(dbg->Control, L"\t%s", buffer);
	}

	dwdprintf(dbg->Control, L"\r\n");
}

DBG_FUNC(dumpstackobjects)
{
	DBG_PREAMBLE;
	ULONG64 stackLimit = 0;
	ULONG64 stackBase = 0;

	auto tokens = SPT::Util::Tokenize(args);
	if (tokens.size() > 0)
	{
		if (tokens.size() > 2 || tokens.size() == 1)
		{
			dwdprintf(dbg.Control, L"Usage: !DumpStackObjects [stackBase [stackLimit]]\r\n");
		}
		else if (tokens.size() == 2)
		{
			stackBase = GetExpression(tokens[0]);
			stackLimit = GetExpression(tokens[1]);
		}
	}

	if (stackLimit == 0)
	{
		CComPtr<IDebugSystemObjects> dso;
		CComPtr<IDebugDataSpaces> dds;
						
		RETURN_IF_FAILED(dbg.Client->QueryInterface(__uuidof(IDebugSystemObjects), (PVOID*)&dso));
		RETURN_IF_FAILED(dbg.Client->QueryInterface(__uuidof(IDebugDataSpaces), (PVOID*)&dds));
		
		CLRDATA_ADDRESS threadTeb;
		RETURN_IF_FAILED(dso->GetCurrentThreadTeb(&threadTeb));
		RETURN_IF_FAILED(CSDbgExt::GetThreadLimitsFromTEB(dds, threadTeb, &stackBase, &stackLimit));
	}

	std::vector<Register> registerValues;
	GetArchRegisters(&dbg, &registerValues, &stackLimit);

	dwdprintf(dbg.Control, L"ESP/REG  Object   Name\r\n");

	std::for_each(registerValues.begin(), registerValues.end(), [&dbg](Register r) -> void {
		if (dbg.Process->IsValidObject(r.Value))
		{
			dwdprintf(dbg.Control, L"%-8s", r.Name);
			ClrObjectData od = {};
			dbg.XCLR->GetObjectData(r.Value, &od);
			od.ObjectAddress = r.Value;
			HandleObject(&dbg, od);
		}
	});

	CComObject<EnumObjectsCallbackAdaptor> adapt;
	adapt.Init([&dbg](ClrObjectData o) -> bool {
		dwdprintf(dbg.Control, L"%p", o.Reserved);
		HandleObject(&dbg, o);
		return TRUE;
	});

	dbg.Ext->EnumStackObjectsByStackParams(stackBase, stackLimit, &adapt);

	return S_OK;
}