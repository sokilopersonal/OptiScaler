#pragma once
#include "SysUtils.h"
#include "Config.h"
#include "detours/detours.h"

const HKEY signatureMark = (HKEY) 0xFFFFFFFF13372137;

typedef decltype(&RegOpenKeyExW) PFN_RegOpenKeyExW;
typedef decltype(&RegEnumValueW) PFN_RegEnumValueW;
typedef decltype(&RegCloseKey) PFN_RegCloseKey;
typedef decltype(&RegQueryValueExW) PFN_RegQueryValueExW;
typedef decltype(&RegQueryValueExA) PFN_RegQueryValueExA;

static PFN_RegOpenKeyExW o_RegOpenKeyExW = nullptr;
static PFN_RegEnumValueW o_RegEnumValueW = nullptr;
static PFN_RegCloseKey o_RegCloseKey = nullptr;
static PFN_RegQueryValueExW o_RegQueryValueExW = nullptr;
static PFN_RegQueryValueExA o_RegQueryValueExA = nullptr;

static LSTATUS hkRegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
{
    LSTATUS result = 0;

    if (lpSubKey != nullptr && (wcscmp(L"SOFTWARE\\NVIDIA Corporation\\Global", lpSubKey) == 0 ||
                                wcscmp(L"SYSTEM\\ControlSet001\\Services\\nvlddmkm", lpSubKey) == 0))
    {
        *phkResult = signatureMark;
        return 0;
    }

    return o_RegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
}

static LSTATUS hkRegEnumValueW(HKEY hKey, DWORD dwIndex, LPWSTR lpValueName, LPDWORD lpcchValueName, LPDWORD lpReserved,
                               LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    if (hKey != signatureMark)
        return o_RegEnumValueW(hKey, dwIndex, lpValueName, lpcchValueName, lpReserved, lpType, lpData, lpcbData);

    if (dwIndex == 0)
    {
        if (lpValueName && lpcchValueName && lpData && lpcbData)
        {
            auto key = L"{41FCC608-8496-4DEF-B43E-7D9BD675A6FF}";
            auto value = 0x01;

            auto keyLength = (DWORD) wcslen(key);

            if (*lpcchValueName <= keyLength)
                return ERROR_MORE_DATA;

            wcsncpy(lpValueName, key, *lpcchValueName);
            lpValueName[*lpcchValueName - 1] = L'\0';
            *lpcchValueName = keyLength;

            if (lpType)
                *lpType = REG_BINARY;

            if (*lpcbData > 0)
            {
                lpData[0] = value;
                *lpcbData = 1;
            }
            else
            {
                return ERROR_MORE_DATA;
            }

            return ERROR_SUCCESS;
        }

        return ERROR_INVALID_PARAMETER;
    }
    else
    {
        return ERROR_NO_MORE_ITEMS;
    }
}

static LSTATUS hkRegCloseKey(HKEY hKey)
{
    if (hKey == signatureMark)
        return ERROR_SUCCESS;

    return o_RegCloseKey(hKey);
}

// Original implementation:
// https://github.com/artur-graniszewski/dlss-enabler-main/blob/1f8b24722f1b526ffb896ae62b6aa3ca766b0728/Utils/RegistryProxy.cpp#L137
static LONG hkRegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData,
                               LPDWORD lpcbData)
{
    static std::wstring vendorId = std::format(L"VEN_{:04X}", Config::Instance()->SpoofedVendorId.value_or_default());
    static std::wstring deviceId = std::format(L"DEV_{:04X}", Config::Instance()->SpoofedDeviceId.value_or_default());
    std::wstring valueName = L"";

    if (lpValueName != NULL)
    {
        valueName = std::wstring(lpValueName);
    }

    if (Config::Instance()->SpoofHAGS.value_or_default() && valueName == L"HwSchMode")
    {
        // Check if lpcbData is not NULL
        if (lpcbData != nullptr)
        {
            // If lpData is NULL, we're being asked for the required size
            if (lpData == nullptr)
            {
                *lpcbData = sizeof(DWORD); // Indicate the required size

                // Set the type to REG_DWORD
                if (lpType)
                {
                    *lpType = REG_DWORD;
                }
                return ERROR_SUCCESS;
            }

            // Check if the buffer is large enough
            if (*lpcbData >= sizeof(DWORD))
            {
                *(DWORD*) lpData = 2;

                // Set the type to REG_DWORD
                if (lpType)
                {
                    *lpType = REG_DWORD;
                }

                // Set the size of the data returned
                *lpcbData = sizeof(DWORD);

                // Return success
                return ERROR_SUCCESS;
            }
            else
            {
                // Buffer is too small, return required size
                *lpcbData = sizeof(DWORD);
                return ERROR_MORE_DATA;
            }
        }

        // If lpcbData is NULL, return an error
        return ERROR_INVALID_PARAMETER;
    }

    // Call the original function
    auto result = o_RegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);

    // Check the result of the query and if the valueName matches
    if (result == ERROR_SUCCESS && Config::Instance()->SpoofRegistry.value_or_default())
    {
        if (valueName == L"DriverVersion")
        {
            const std::wstring spoofedValue = Config::Instance()->SpoofedDriver.value_or_default();

            size_t spoofedValueSize =
                (spoofedValue.size() + 1) * sizeof(wchar_t); // Size in bytes including null terminator

            if (lpData != nullptr && lpcbData != nullptr)
            {
                // Check if buffer size is sufficient
                if (*lpcbData >= spoofedValueSize)
                {
                    // Copy the spoofed value into lpData
                    std::memcpy(lpData, spoofedValue.c_str(), spoofedValueSize);
                    // Update lpcbData with the size of the spoofed value
                    *lpcbData = static_cast<DWORD>(spoofedValueSize);

                    LOG_INFO("New DriverVersion: {}", wstring_to_string(spoofedValue));
                }
                else
                {
                    // If buffer is too small, set lpcbData to the required size
                    *lpcbData = static_cast<DWORD>(spoofedValueSize);
                    result = ERROR_MORE_DATA; // Indicate that buffer was too small
                }
            }
        }

        if (valueName == L"HardwareID")
        {
            // Handle REG_SZ type
            std::wstring data(reinterpret_cast<wchar_t*>(lpData), *lpcbData / sizeof(wchar_t));
            std::wstring newData = data;
            size_t pos = 0;
            bool found = false;

            // Replace VEN_1002 and VEN_8086 with VEN_10DE in REG_SZ
            while ((pos = newData.find(L"VEN_1002", pos)) != std::wstring::npos)
            {
                newData.replace(pos, 8, vendorId);
                pos += 8; // Move past the replacement
                found = true;
            }

            pos = 0;
            while ((pos = newData.find(L"VEN_8086", pos)) != std::wstring::npos)
            {
                newData.replace(pos, 8, vendorId);
                pos += 8; // Move past the replacement
                found = true;
            }

            if (found)
            {
                // Replace Device Id
                pos = 0;
                while ((pos = newData.find(L"DEV_", pos)) != std::wstring::npos)
                {
                    newData.replace(pos, 8, deviceId);
                    pos += 8; // Move past the replacement
                }

                LOG_INFO("New HardwareID: {}", wstring_to_string(newData));
            }

            // Copy the new data back
            wcscpy_s(reinterpret_cast<wchar_t*>(lpData), *lpcbData / sizeof(wchar_t), newData.c_str());
        }
    }

    return result;
}

LONG WINAPI hkRegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData,
                               LPDWORD lpcbData)
{
    static std::string vendorId = std::format("VEN_{:04X}", Config::Instance()->SpoofedVendorId.value_or_default());
    static std::string deviceId = std::format("DEV_{:04X}", Config::Instance()->SpoofedDeviceId.value_or_default());
    std::string valueName = "";

    if (lpValueName != NULL)
    {
        valueName = std::string(lpValueName);
    }

    if (Config::Instance()->SpoofHAGS.value_or_default() && valueName == "HwSchMode")
    {
        // Check if lpcbData is not NULL
        if (lpcbData != nullptr)
        {
            // If lpData is NULL, we're being asked for the required size
            if (lpData == nullptr)
            {
                *lpcbData = sizeof(DWORD); // Indicate the required size

                // Set the type to REG_DWORD
                if (lpType)
                {
                    *lpType = REG_DWORD;
                }
                return ERROR_SUCCESS;
            }

            // Check if the buffer is large enough
            if (*lpcbData >= sizeof(DWORD))
            {
                *(DWORD*) lpData = 2;

                // Set the type to REG_DWORD
                if (lpType)
                {
                    *lpType = REG_DWORD;
                }

                // Set the size of the data returned
                *lpcbData = sizeof(DWORD);

                // Return success
                return ERROR_SUCCESS;
            }
            else
            {
                // Buffer is too small, return required size
                *lpcbData = sizeof(DWORD);
                return ERROR_MORE_DATA;
            }
        }

        // If lpcbData is NULL, return an error
        return ERROR_INVALID_PARAMETER;
    }

    // Call the original function
    auto result = o_RegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);

    // Check the result of the query and if the valueName matches
    if (result == ERROR_SUCCESS && Config::Instance()->SpoofRegistry.value_or_default())
    {
        if (valueName == "DriverVersion")
        {
            const std::string spoofedValue = wstring_to_string(Config::Instance()->SpoofedDriver.value_or_default());
            size_t spoofedValueSize =
                (spoofedValue.size() + 1) * sizeof(wchar_t); // Size in bytes including null terminator

            if (lpData != nullptr && lpcbData != nullptr)
            {
                // Check if buffer size is sufficient
                if (*lpcbData >= spoofedValueSize)
                {
                    // Copy the spoofed value into lpData
                    std::memcpy(lpData, spoofedValue.c_str(), spoofedValueSize);
                    // Update lpcbData with the size of the spoofed value
                    *lpcbData = static_cast<DWORD>(spoofedValueSize);

                    LOG_INFO("New DriverVersion: {}", spoofedValue);
                }
                else
                {
                    // If buffer is too small, set lpcbData to the required size
                    *lpcbData = static_cast<DWORD>(spoofedValueSize);
                    result = ERROR_MORE_DATA; // Indicate that buffer was too small
                }
            }
        }

        if (valueName == "HardwareID")
        {
            // Handle REG_SZ type
            std::string data(reinterpret_cast<char*>(lpData), *lpcbData / sizeof(char));
            std::string newData = data;
            size_t pos = 0;
            bool found = false;

            // Replace VEN_1002 and VEN_8086 with VEN_10DE in REG_SZ
            while ((pos = newData.find("VEN_1002", pos)) != std::wstring::npos)
            {
                newData.replace(pos, 8, vendorId);
                pos += 8; // Move past the replacement
                found = true;
            }

            pos = 0;
            while ((pos = newData.find("VEN_8086", pos)) != std::wstring::npos)
            {
                newData.replace(pos, 8, vendorId);
                pos += 8; // Move past the replacement
                found = true;
            }

            // Replace Device Id
            if (found)
            {
                pos = 0;
                while ((pos = newData.find("DEV_", pos)) != std::string::npos)
                {
                    newData.replace(pos, 8, deviceId);
                    pos += 8; // Move past the replacement
                }

                LOG_INFO("New HardwareID: {}", newData);
            }

            // Copy the new data back
            std::memcpy(lpData, newData.c_str(), *lpcbData);
        }
    }

    return result;
}

static void hookAdvapi32()
{
    LOG_FUNC();

    o_RegOpenKeyExW = reinterpret_cast<PFN_RegOpenKeyExW>(DetourFindFunction("Advapi32.dll", "RegOpenKeyExW"));
    o_RegEnumValueW = reinterpret_cast<PFN_RegEnumValueW>(DetourFindFunction("Advapi32.dll", "RegEnumValueW"));
    o_RegCloseKey = reinterpret_cast<PFN_RegCloseKey>(DetourFindFunction("Advapi32.dll", "RegCloseKey"));

    if (Config::Instance()->SpoofHAGS.value_or_default() || Config::Instance()->SpoofRegistry.value_or_default())
    {
        o_RegQueryValueExW =
            reinterpret_cast<PFN_RegQueryValueExW>(DetourFindFunction("Advapi32.dll", "RegQueryValueExW"));
        o_RegQueryValueExA =
            reinterpret_cast<PFN_RegQueryValueExA>(DetourFindFunction("Advapi32.dll", "RegQueryValueExA"));
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_RegOpenKeyExW)
        DetourAttach(&(PVOID&) o_RegOpenKeyExW, hkRegOpenKeyExW);

    if (o_RegEnumValueW)
        DetourAttach(&(PVOID&) o_RegEnumValueW, hkRegEnumValueW);

    if (o_RegCloseKey)
        DetourAttach(&(PVOID&) o_RegCloseKey, hkRegCloseKey);

    if (o_RegQueryValueExW)
        DetourAttach(&(PVOID&) o_RegQueryValueExW, hkRegQueryValueExW);

    if (o_RegQueryValueExA)
        DetourAttach(&(PVOID&) o_RegQueryValueExA, hkRegQueryValueExA);

    DetourTransactionCommit();
}

static void unhookAdvapi32()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_RegOpenKeyExW)
    {
        DetourDetach(&(PVOID&) o_RegOpenKeyExW, hkRegOpenKeyExW);
        o_RegOpenKeyExW = nullptr;
    }

    if (o_RegEnumValueW)
    {
        DetourDetach(&(PVOID&) o_RegEnumValueW, hkRegEnumValueW);
        o_RegEnumValueW = nullptr;
    }

    if (o_RegCloseKey)
    {
        DetourDetach(&(PVOID&) o_RegCloseKey, hkRegCloseKey);
        o_RegCloseKey = nullptr;
    }

    DetourTransactionCommit();
}
