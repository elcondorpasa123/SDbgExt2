#pragma once
#include "stdafx.h"
#include "ClrObjectArray.h"

class ClrObject : public IClrObject
{
public:
	ClrObject(IClrProcess *proc, CLRDATA_ADDRESS obj)
		: m_proc(proc), m_addr(obj), m_ref(1), m_mtAddr(0)
	{
	}
	
	STDMETHODIMP_(ULONG) AddRef() { return ++m_ref; }
	STDMETHODIMP_(ULONG) Release()
	{
		ULONG newRef = --m_ref;
		if (newRef == 0)
			delete this;
		return newRef;
	}

	STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject)
    {
		IUnknown *punk = nullptr;

        if (riid == IID_IUnknown)
            punk = static_cast<IUnknown*>(this);
		else if (riid == __uuidof(IClrObject))
			punk = static_cast<IClrObject*>(this);

        *ppvObject = punk;
        if (!punk)
            return E_NOINTERFACE;

        punk->AddRef();
        return S_OK;
    }

	STDMETHODIMP_(LONG) IsValid() 
	{
		return m_addr != NULL && m_proc->IsValidObject(m_addr);
	}

	STDMETHODIMP_(CLRDATA_ADDRESS) Address()
	{
		return m_addr;
	}

	STDMETHODIMP GetFieldValueAddr(LPWSTR field, CLRDATA_ADDRESS *ret)
	{
		return m_proc->GetFieldValuePtr(m_addr, field, ret);
	}

	STDMETHODIMP GetFieldValueObj(LPWSTR field, IClrObject **ret)
	{
		CLRDATA_ADDRESS addr = 0;
		HRESULT hr = S_OK;
		RETURN_IF_FAILED(m_proc->GetFieldValuePtr(m_addr, field, &addr));

		*ret = new ClrObject(m_proc, addr);	
		return S_OK;
	}

	STDMETHODIMP GetFieldValueUInt32(LPWSTR field, UINT32 *val)
	{
		HRESULT hr = S_OK;
		return m_proc->GetFieldValueBuffer(m_addr, field, sizeof(UINT32), (PVOID)val, NULL);
	}

	STDMETHODIMP GetFieldValueWSTR(LPWSTR field, ULONG32 iNumChars, LPWSTR buffer, PULONG bytesRead)
	{
		HRESULT hr = S_OK;
		return m_proc->GetFieldValueString(m_addr, field, iNumChars, buffer, bytesRead);
	}

	STDMETHODIMP GetFieldValueArray(LPWSTR field, IClrObjectArray **ret)
	{
		HRESULT hr = S_OK;
		CLRDATA_ADDRESS arrayObj;
		RETURN_IF_FAILED(m_proc->GetFieldValuePtr(m_addr, field, &arrayObj));
		/*if (arrayObj == NULL)
		{
			*ret = NULL;
			return S_FALSE;
		}
		// TODO: Assert this is a real array?
		ClrObjectData od = {};
		m_proc->GetProcess()->GetObjectData(arrayObj, &od);
		*/

		*ret = new ClrObjectArray(m_proc, arrayObj);
		return S_OK;
	}

	STDMETHODIMP GetTypeName(ULONG32 cchBuffer, LPWSTR buffer, PULONG nameLen)
	{
		HRESULT hr = S_OK;

		if (!m_mtAddr)
		{
			ClrObjectData od = {};
			RETURN_IF_FAILED(m_proc->GetProcess()->GetObjectData(m_addr, &od));

			m_mtAddr = od.MethodTable;
		}

		return m_proc->GetProcess()->GetMethodTableName(m_mtAddr, cchBuffer, buffer, (ULONG32*)nameLen);
	}

private:
	int m_ref;
	CComPtr<IClrProcess> m_proc;
	CLRDATA_ADDRESS m_addr;
	CLRDATA_ADDRESS m_mtAddr;
};