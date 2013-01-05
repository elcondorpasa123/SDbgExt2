#include "stdafx.h"
#include "DictionaryEnumerator.h"

HRESULT DctEnumerator::EnumerateDctEntries(CLRDATA_ADDRESS dctObj, IEnumHashtableCallback *cb)
{
	CComPtr<IXCLRDataProcess3> proc;
	m_dac->GetProcess(&proc);

	ClrObjectData od = {};
	ClrFieldDescData fd = {};
	CLRDATA_ADDRESS field;
	HRESULT hr = S_OK;
	RETURN_IF_FAILED(proc->GetObjectData(dctObj, &od));

	// Dictionary<K,V>?
	hr = m_dac->FindFieldByName(od.MethodTable, L"entries", &field, &fd);
	if (SUCCEEDED(hr))
	{
		return EnumerateDctEntriesImpl(dctObj, od.MethodTable, L"entries", L"key", L"value", L"hashCode", cb);
	}

	// Hashtable?
	hr = m_dac->FindFieldByName(od.MethodTable, L"buckets", &field, &fd);
	if (SUCCEEDED(hr))
	{
		return EnumerateDctEntriesImpl(dctObj, od.MethodTable, L"buckets", L"key", L"val", L"hash_coll", cb);
	}

	WCHAR typeName[53];
	hr = proc->GetMethodTableName(od.MethodTable, ARRAYSIZE(typeName), typeName, NULL);
	if (SUCCEEDED(hr) && wcscmp(typeName, L"System.Collections.Specialized.HybridDictionary") == 0)
	{
		CLRDATA_ADDRESS htPtr = NULL;
		hr = m_dac->GetFieldValuePtr(dctObj, L"hashtable", &htPtr);
		if (SUCCEEDED(hr) && htPtr != NULL)
		{
			return EnumerateDctEntries(htPtr, cb);
		}
		// It might still have data in it's list field
		hr = m_dac->GetFieldValuePtr(dctObj, L"list", &htPtr);
		if (SUCCEEDED(hr) && htPtr != NULL)
		{
			return EnumerateDctEntries(htPtr, cb);
		}
		else
		{
			return E_INVALIDARG;
		}
	}
	else if (SUCCEEDED(hr) && wcscmp(typeName, L"System.Collections.Specialized.ListDictionary") == 0)
	{
		return EnumerateHybridListEntries(dctObj, cb);
	}
	else if (SUCCEEDED(hr) && wcsncmp(typeName, L"System.Collections.Concurrent.ConcurrentDictionary`2", 52) == 0)
	{
		return EnumerateConcurrentDictionaryEntries(dctObj, cb);
	}
	else
	{
		return E_INVALIDARG;
	}
}

HRESULT DctEnumerator::EnumerateDctEntriesImpl(CLRDATA_ADDRESS dctObj, CLRDATA_ADDRESS methodTable
												, WCHAR *bucketsName, WCHAR *keyFieldName, WCHAR *valFieldName, WCHAR *hashFieldName, IEnumHashtableCallback *cb)
{
	CLRDATA_ADDRESS entriesPtr = NULL;
	auto hr = m_dac->GetFieldValuePtr(dctObj, bucketsName, &entriesPtr);

	ClrFieldDescData bucketField = {};
	RETURN_IF_FAILED(m_dac->FindFieldByName(methodTable, bucketsName, NULL, &bucketField));

	DWORD arrayEntries = 0;
	ULONG keyOffset, valOffset, hashCodeOffset, arrayElementSize;
	CLRDATA_ADDRESS arrayDataBase;

	BOOL elementsAreClass;
	hr = GetEntryOffsets(entriesPtr, keyFieldName, valFieldName, hashFieldName, &keyOffset, &valOffset, &hashCodeOffset, 
							&arrayDataBase, &arrayElementSize, &arrayEntries, &elementsAreClass);

	return ReadEntries(arrayEntries, entriesPtr, arrayDataBase, arrayElementSize, keyOffset, valOffset, hashCodeOffset, elementsAreClass, cb);
}

HRESULT DctEnumerator::GetEntryOffsets(CLRDATA_ADDRESS entriesPtr, WCHAR *keyFieldName, WCHAR *valFieldName, WCHAR *hashFieldName, 
									   ULONG *keyOffset, ULONG *valueOffset, ULONG *hashCodeOffset, 
									   CLRDATA_ADDRESS *arrayBase, ULONG *arrayElementSize, ULONG *arrayEntries, BOOL *elementsAreClass)
{
	ClrObjectData entriesData = {};
	CComPtr<IXCLRDataProcess3> dac;
	m_dac->GetProcess(&dac);
	dac->GetObjectData(entriesPtr, &entriesData);
	CLRDATA_ADDRESS typeMt = entriesData.ArrayData.ElementMethodTable;
	ClrMethodTableData elementMtData = {};
	dac->GetMethodTableData(typeMt, &elementMtData);
	*elementsAreClass = ((elementMtData.ClassAttributes & 0x100) == 0);

	ULONG baseOffset = (*elementsAreClass) ? sizeof(void*) : 0;

	ClrFieldDescData field;
	HRESULT hr = S_OK;
	RETURN_IF_FAILED(m_dac->FindFieldByName(typeMt, keyFieldName, NULL, &field))
	*keyOffset = field.Offset + baseOffset;

	RETURN_IF_FAILED(m_dac->FindFieldByName(typeMt, valFieldName, NULL, &field))
	*valueOffset = field.Offset + baseOffset;
	
	RETURN_IF_FAILED(m_dac->FindFieldByName(typeMt, hashFieldName, NULL, &field));
	*hashCodeOffset = field.Offset + baseOffset;

	*arrayBase = entriesData.ArrayData.FirstElement;
	*arrayElementSize = entriesData.ArrayData.ElementSize;
	*arrayEntries = entriesData.ArrayData.NumElements;

	return S_OK;
}

HRESULT DctEnumerator::ReadEntries(DWORD arrayEntries, CLRDATA_ADDRESS bucketArrayBase, CLRDATA_ADDRESS arrayDataBase, ULONG arrayElementSize,
									ULONG keyOffset, ULONG valOffset, ULONG hashCodeOffset, BOOL elementsAreClass, IEnumHashtableCallback *cb)
{	
	for (DWORD a = 0; a < arrayEntries; a++)
	{
		CLRDATA_ADDRESS arrayDataPtr = arrayDataBase + (arrayElementSize*a);
		HRESULT hr = ReadEntry(keyOffset, valOffset, hashCodeOffset, bucketArrayBase, arrayDataPtr, elementsAreClass, cb);

		if (hr == S_FALSE)
			return S_OK;
	}
	return S_OK;
}


HRESULT DctEnumerator::ReadEntry(ULONG keyOffset, ULONG valueOffset, ULONG hashCodeOffset, CLRDATA_ADDRESS bucketArrayBase, CLRDATA_ADDRESS arrayDataPtr, BOOL elementIsClass, IEnumHashtableCallback *cb)
{
	HRESULT hr = S_OK;

	PVOID keyValue;
	PVOID valueValue;
	ULONG32 hashValue;

	ULONG bytesRead = 0;

	CComPtr<IDacMemoryAccess> dcma;
	m_dac->GetDataAccess(&dcma);

	ULONG64 baseAddr = arrayDataPtr;
	if (elementIsClass)
	{
		CLRDATA_ADDRESS newBaseAddr = 0;
		if (FAILED(dcma->ReadVirtual(baseAddr, &newBaseAddr, sizeof(PVOID), NULL)) || !newBaseAddr)
		{
			return S_OK;
		}
		else
		{
			baseAddr = newBaseAddr;
		}
	}
	
	hr = dcma->ReadVirtual(baseAddr+keyOffset, &keyValue, sizeof(PVOID), &bytesRead);
	if (FAILED(hr)) return S_OK;
	hr = dcma->ReadVirtual(baseAddr+valueOffset, &valueValue, sizeof(PVOID), &bytesRead);
	if (FAILED(hr)) return S_OK;
	hr = dcma->ReadVirtual(baseAddr+hashCodeOffset, &hashValue, sizeof(ULONG32), &bytesRead);
	if (FAILED(hr)) return S_OK;
	
	if (!(keyValue == 0 && valueValue == 0 && hashValue == 0))
	{
		if ((CLRDATA_ADDRESS)keyValue != bucketArrayBase) //unused bucket
		{
			DctEntry entry = { baseAddr, (CLRDATA_ADDRESS)keyValue, (CLRDATA_ADDRESS)valueValue, hashValue };
			if (FAILED(cb->Callback(entry)))
				return S_FALSE;
		}
	}
	return S_OK;
}

HRESULT DctEnumerator::EnumerateConcurrentDictionaryEntries(CLRDATA_ADDRESS dctObj, IEnumHashtableCallback *cb)
{
	IXCLRDataProcess3Ptr xclr;
	m_dac->GetProcess(&xclr);
	ClrObjectData od = {};

	CLRDATA_ADDRESS tables;
	if (SUCCEEDED(m_dac->GetFieldValuePtr(dctObj, L"m_tables", &tables))
		&& SUCCEEDED(xclr->GetObjectData(tables, &od))
		)
	{
		return this->EnumerateDctEntriesImpl(tables, od.MethodTable, L"m_buckets", L"m_key", L"m_value", L"m_hashcode", cb);
	}
	return E_INVALIDARG;
}

HRESULT DctEnumerator::EnumerateHybridListEntries(CLRDATA_ADDRESS listObj, IEnumHashtableCallback *cb)
{
	ClrObjectData od = {};
	HRESULT hr = S_OK;
	CComPtr<IXCLRDataProcess3> dac;
	m_dac->GetProcess(&dac);
	RETURN_IF_FAILED(dac->GetObjectData(listObj, &od));

	CLRDATA_ADDRESS currNode = NULL;
	RETURN_IF_FAILED(m_dac->GetFieldValuePtr(listObj, L"head", &currNode));

	if (!currNode)
	{
		return S_OK;
	}

	RETURN_IF_FAILED(dac->GetObjectData(currNode, &od));

	ClrFieldDescData keyField, valueField, nextField;
	RETURN_IF_FAILED(m_dac->FindFieldByName(od.MethodTable, L"key", NULL, &keyField))
	RETURN_IF_FAILED(m_dac->FindFieldByName(od.MethodTable, L"value", NULL, &valueField));
	RETURN_IF_FAILED(m_dac->FindFieldByName(od.MethodTable, L"next", NULL, &nextField));

	CComPtr<IDacMemoryAccess> dcma;
	m_dac->GetDataAccess(&dcma);

	ULONG bytesRead = 0;
	while(currNode)
	{
		CLRDATA_ADDRESS keyValue = 0, valueValue = 0, nextValue = 0;
		hr = dcma->ReadVirtual(currNode + sizeof(PVOID) + nextField.Offset, &nextValue, sizeof(PVOID), &bytesRead);
		if (FAILED(hr)) return hr;

		hr = dcma->ReadVirtual(currNode + sizeof(PVOID) + keyField.Offset, &keyValue, sizeof(PVOID), &bytesRead);
		if (FAILED(hr)) 
		{
			currNode = nextValue;
			continue;
		}
		hr = dcma->ReadVirtual(currNode + sizeof(PVOID) + valueField.Offset, &valueValue, sizeof(PVOID), &bytesRead);
		if (FAILED(hr)) 
		{
			currNode = nextValue;
			continue;
		}

		DctEntry ent = { currNode, keyValue, valueValue, 0 }; 
		cb->Callback(ent);

		currNode = nextValue;
	}	

	return S_OK;
}

HRESULT DctEnumerator::FindDctEntryByKey(CLRDATA_ADDRESS dctObj, LPCWSTR key, CLRDATA_ADDRESS *targetAddr)
{
	HRESULT hr = S_OK;

	struct DctKeySearchState
	{
		CComPtr<IXCLRDataProcess3> pDac;
		LPCWSTR TargetKey;
		size_t TargetKeyLen;
		CLRDATA_ADDRESS TargetValuePtr;
	};

	CComPtr<IXCLRDataProcess3> dac;
	m_dac->GetProcess(&dac);

	DctKeySearchState dkss = { dac, key, wcslen(key), NULL };
	auto cb = [&dkss](DctEntry entry)->BOOL {
		BOOL ret = TRUE;
		std::wstring keyStr(dkss.TargetKeyLen + 1, '\0');

		if (SUCCEEDED(dkss.pDac->GetObjectStringData(entry.KeyPtr, (UINT)dkss.TargetKeyLen + 1, &keyStr[0], NULL)))
		{
			if (keyStr.compare(0, dkss.TargetKeyLen, dkss.TargetKey) == 0)
			{
				dkss.TargetValuePtr = entry.ValuePtr;
				return FALSE;
			}
		}
		
		return TRUE;
	};
	
	CComObject<EnumDctAdaptor> adapt;
	adapt.Init(cb);

	EnumerateDctEntries(dctObj, &adapt);
	*targetAddr = dkss.TargetValuePtr;

	return dkss.TargetValuePtr != NULL ? S_OK : E_FAIL;
}

HRESULT DctEnumerator::FindDctEntryByHash(CLRDATA_ADDRESS dctObj, UINT32 hash, CLRDATA_ADDRESS *targetAddr)
{
	HRESULT hr = S_OK;
	
	struct DctHashSearchState
	{
		UINT32 TargetHash;
		CLRDATA_ADDRESS TargetValuePtr;
	};

	DctHashSearchState dhss = { hash, NULL };
	auto cb = [&dhss](DctEntry entry)->BOOL {
		if (entry.HashCode == dhss.TargetHash)
		{
			dhss.TargetValuePtr = entry.ValuePtr;
			return FALSE;
		}
		return TRUE;
	};

	CComObject<EnumDctAdaptor> adapt;
	adapt.Init(cb);

	EnumerateDctEntries(dctObj, &adapt);
	*targetAddr = dhss.TargetValuePtr;

	return dhss.TargetValuePtr != NULL ? S_OK : E_FAIL;
}
