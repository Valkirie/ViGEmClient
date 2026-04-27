/*
MIT License

Copyright (c) 2017-2019 Nefarius Software Solutions e.U. and Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


//
// WinAPI
// 
#include <Windows.h>
#include <SetupAPI.h>
#include <initguid.h>
#include <Dbghelp.h>
#include <devpkey.h>
#include <strsafe.h>

//
// Driver shared
// 
#include "ViGEm/km/BusShared.h"
#include "ViGEm/Client.h"
#include <winioctl.h>

//
// STL
// 
#include <cstdlib>
#include <climits>
#include <thread>
#include <functional>
#include <string>

//
// Internal
// 
#include "Internal.h"

//#define VIGEM_VERBOSE_LOGGING_ENABLED


#pragma region Diagnostics

#ifdef _DEBUG
#define DBGPRINT(kwszDebugFormatString, ...) _DBGPRINT(__FUNCTIONW__, __LINE__, kwszDebugFormatString, __VA_ARGS__)
#else
#define DBGPRINT( kwszDebugFormatString, ... ) ;;
#endif

VOID _DBGPRINT(LPCWSTR kwszFunction, INT iLineNumber, LPCWSTR kwszDebugFormatString, ...)
{
#if defined(VIGEM_VERBOSE_LOGGING_ENABLED)
	INT cbFormatString = 0;
	va_list args;
	PWCHAR wszDebugString = nullptr;
	size_t st_Offset = 0;

	va_start(args, kwszDebugFormatString);

	cbFormatString = _scwprintf(L"[%s:%d] ", kwszFunction, iLineNumber) * sizeof(WCHAR);
	cbFormatString += _vscwprintf(kwszDebugFormatString, args) * sizeof(WCHAR) + 2;

	/* Depending on the size of the format string, allocate space on the stack or the heap. */
	wszDebugString = static_cast<PWCHAR>((0));

	/* Populate the buffer with the contents of the format string. */
	StringCbPrintfW(wszDebugString, cbFormatString, L"[%s:%d] ", kwszFunction, iLineNumber);
	StringCbLengthW(wszDebugString, cbFormatString, &st_Offset);
	StringCbVPrintfW(&wszDebugString[st_Offset / sizeof(WCHAR)], cbFormatString - st_Offset, kwszDebugFormatString,
		args);

	OutputDebugStringW(wszDebugString);

	_freea(wszDebugString);
	va_end(args);
#else
	std::ignore = kwszFunction;
	std::ignore = iLineNumber;
	std::ignore = kwszDebugFormatString;
#endif
}

static void to_hex(unsigned char* in, size_t insz, char* out, size_t outsz)
{
	unsigned char* pin = in;
	auto hex = "0123456789ABCDEF";
	char* pout = out;
	for (; pin < in + insz; pout += 3, pin++)
	{
		pout[0] = hex[(*pin >> 4) & 0xF];
		pout[1] = hex[*pin & 0xF];
		pout[2] = ':';
		if (static_cast<size_t>(pout + 3 - out) > outsz)
		{
			/* Better to truncate output string than overflow buffer */
			/* it would be still better to either return a status */
			/* or ensure the target buffer is large enough and it never happen */
			break;
		}
	}
	pout[-1] = 0;
}

#pragma endregion

//
// Uncomment to compile in crash dump handler
// 
//#define VIGEM_USE_CRASH_HANDLER


#ifdef VIGEM_USE_CRASH_HANDLER
typedef BOOL(WINAPI* MINIDUMPWRITEDUMP)(
	HANDLE hProcess,
	DWORD dwPid,
	HANDLE hFile,
	MINIDUMP_TYPE DumpType,
	CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
	CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
	CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam
	);

LONG WINAPI vigem_internal_exception_handler(struct _EXCEPTION_POINTERS* apExceptionInfo);
#endif

//
// DeviceIOControl request notification handler classes for X360 and DS4 controller types. 
// vigem_target_XXX_register_notification functions use x360 or DS4 derived class instances in a notification thread handlers.
//
class NotificationRequestPayload
{
public:
	LPVOID lpPayloadBuffer;
	DWORD  payloadBufferSize;
	DWORD  ioControlCode;

public:
	NotificationRequestPayload(DWORD _bufferSize, DWORD _ioControlCode)
	{
		lpPayloadBuffer = malloc(_bufferSize);
		payloadBufferSize = _bufferSize;
		ioControlCode = _ioControlCode;
	}

	virtual ~NotificationRequestPayload()
	{
		free(lpPayloadBuffer);
	}

	virtual void ProcessNotificationRequest(PVIGEM_CLIENT client, PVIGEM_TARGET target) = 0;
};

class NotificationRequestPayloadX360 : public NotificationRequestPayload
{
public:
	NotificationRequestPayloadX360(ULONG _serialNo) : NotificationRequestPayload(sizeof(XUSB_REQUEST_NOTIFICATION), IOCTL_XUSB_REQUEST_NOTIFICATION)
	{
		// Let base class to allocate required buffer size, but initialize it here with a correct type of initialization function
		XUSB_REQUEST_NOTIFICATION_INIT((PXUSB_REQUEST_NOTIFICATION)lpPayloadBuffer, _serialNo);
	}

	void ProcessNotificationRequest(PVIGEM_CLIENT client, PVIGEM_TARGET target) override
	{
		if (target->Notification != nullptr)
		{
			PXUSB_REQUEST_NOTIFICATION currentNotify = static_cast<PXUSB_REQUEST_NOTIFICATION>(lpPayloadBuffer);
			reinterpret_cast<PFN_VIGEM_X360_NOTIFICATION>(target->Notification)(client, target,
				currentNotify->LargeMotor,
				currentNotify->SmallMotor,
				currentNotify->LedNumber,
				target->NotificationUserData
				);
		}
	}
};

class NotificationRequestPayloadDS4 : public NotificationRequestPayload
{
public:
	NotificationRequestPayloadDS4(ULONG _serialNo) : NotificationRequestPayload(sizeof(DS4_REQUEST_NOTIFICATION), IOCTL_DS4_REQUEST_NOTIFICATION)
	{
		// Let base class to allocate required buffer size, but initialize it here with a correct type of initialization function
		DS4_REQUEST_NOTIFICATION_INIT((PDS4_REQUEST_NOTIFICATION)lpPayloadBuffer, _serialNo);
	}

	void ProcessNotificationRequest(PVIGEM_CLIENT client, PVIGEM_TARGET target) override
	{
		if (target->Notification != nullptr)
		{
			PDS4_REQUEST_NOTIFICATION currentNotify = static_cast<PDS4_REQUEST_NOTIFICATION>(lpPayloadBuffer);
			reinterpret_cast<PFN_VIGEM_DS4_NOTIFICATION>(target->Notification)(client, target,
				currentNotify->Report.LargeMotor,
				currentNotify->Report.SmallMotor,
				currentNotify->Report.LightbarColor,
				target->NotificationUserData
				);
		}
	}
};

struct Version
{
	Version(std::string versionStr)
	{
		sscanf_s(versionStr.c_str(), "%d.%d.%d.%d", &major, &minor, &revision, &build);
	}

	int CompareTo(const Version& otherVersion)
	{
		static int VERSION_EQUAL = 0;
		static int VERSION_LESSTHAN = -1;
		static int VERSION_GREATER = 1;

		int result = VERSION_EQUAL; // Default to equal
		if (major < otherVersion.major)
		{
			result = VERSION_LESSTHAN;
			return result;
		}
		else if (otherVersion.major < major)
		{
			result = VERSION_GREATER;
			return result;
		}

		if (minor < otherVersion.minor)
		{
			result = VERSION_LESSTHAN;
			return result;
		}
		else if (otherVersion.minor < minor)
		{
			result = VERSION_GREATER;
			return result;
		}

		if (revision < otherVersion.revision)
		{
			result = VERSION_LESSTHAN;
			return result;
		}
		else if (otherVersion.revision < revision)
		{
			result = VERSION_GREATER;
			return result;
		}

		if (build < otherVersion.build)
		{
			result = VERSION_LESSTHAN;
			return result;
		}
		else if (otherVersion.build < build)
		{
			result = VERSION_GREATER;
			return result;
		}

		return result;
	}

	bool Empty()
	{
		bool result = major == 0 && minor == 0 && revision == 0 && build == 0;
		return result;
	}

	int major = 0, minor = 0, revision = 0, build = 0;
};

//
// Initializes a virtual gamepad object.
// 
PVIGEM_TARGET FORCEINLINE VIGEM_TARGET_ALLOC_INIT(
	_In_ VIGEM_TARGET_TYPE Type
)
{
	auto target = static_cast<PVIGEM_TARGET>(malloc(sizeof(VIGEM_TARGET)));

	if (!target)
		return nullptr;

	memset(target, 0, sizeof(VIGEM_TARGET));

	target->Size = sizeof(VIGEM_TARGET);
	target->State = VIGEM_TARGET_INITIALIZED;
	target->Type = Type;
	return target;
}

static DWORD WINAPI vigem_internal_ds4_output_report_pickup_handler(LPVOID Parameter)
{
	const auto pClient = static_cast<PVIGEM_CLIENT>(Parameter);
	DS4_AWAIT_OUTPUT await;
	DEVICE_IO_CONTROL_BEGIN;

	DBGPRINT(L"Started DS4 Output Report pickup thread for 0x%p", pClient);

	do
	{
		DS4_AWAIT_OUTPUT_INIT(&await, 0);

		DeviceIoControl(
			pClient->hBusDevice,
			IOCTL_DS4_AWAIT_OUTPUT_AVAILABLE,
			&await,
			await.Size,
			&await,
			await.Size,
			&transferred,
			&lOverlapped
		);

		if (GetOverlappedResult(pClient->hBusDevice, &lOverlapped, &transferred, TRUE) == 0)
		{
			const DWORD error = GetLastError();

			//
			// Backwards compatibility with version pre-1.19, where this IOCTL doesn't exist
			// 
			if (error == ERROR_INVALID_PARAMETER)
			{
				DBGPRINT(L"Currently used driver version doesn't support this request, aborting");
				break;
			}

			DBGPRINT(L"Win32 Error: 0x%X", error);
		}

#if defined(VIGEM_VERBOSE_LOGGING_ENABLED)
		DBGPRINT(L"Dumping buffer for %d", await.SerialNo);

		const PCHAR dumpBuffer = (PCHAR)calloc(sizeof(DS4_OUTPUT_BUFFER), 3);
		to_hex(await.Report.Buffer, sizeof(DS4_OUTPUT_BUFFER), dumpBuffer, sizeof(DS4_OUTPUT_BUFFER) * 3);
		OutputDebugStringA(dumpBuffer);
		free(dumpBuffer);
#endif

		const PVIGEM_TARGET pTarget = pClient->pTargetsList[await.SerialNo];

		if (pTarget)
		{
			memcpy(&pTarget->Ds4CachedOutputReport, &await.Report, sizeof(DS4_OUTPUT_BUFFER));
			SetEvent(pTarget->Ds4CachedOutputReportUpdateAvailable);
		}
		else
		{
			DBGPRINT(L"No target to report to for serial %d", await.SerialNo);
		}
	} while (WaitForSingleObjectEx(pClient->hDS4OutputReportPickupThreadAbortEvent, 0, FALSE) == WAIT_TIMEOUT);

	DEVICE_IO_CONTROL_END;

	DBGPRINT(L"Finished DS4 Output Report pickup thread for 0x%p", pClient);

	return 0;
}

PVIGEM_CLIENT vigem_alloc()
{
	const auto driver = static_cast<PVIGEM_CLIENT>(malloc(sizeof(VIGEM_CLIENT)));

	if (!driver)
		return nullptr;

	RtlZeroMemory(driver, sizeof(VIGEM_CLIENT));
	driver->hBusDevice = INVALID_HANDLE_VALUE;
	driver->hDS4OutputReportPickupThreadAbortEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

	return driver;
}

void vigem_free(PVIGEM_CLIENT vigem)
{
	if (vigem)
		free(vigem);
}

#define CONNECT_VERSION_CHECK_TRIES 4
#define CONNECT_VERSION_CHECK_DELAY_MS 500

VIGEM_ERROR vigem_connect(PVIGEM_CLIENT vigem)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	SP_DEVICE_INTERFACE_DATA deviceInterfaceData = { 0 };
	deviceInterfaceData.cbSize = sizeof(deviceInterfaceData);
	DWORD memberIndex = 0;
	DWORD requiredSize = 0;
	auto error = VIGEM_ERROR_BUS_NOT_FOUND;

	// check for already open handle as re-opening accidentally would destroy all live targets
	if (vigem->hBusDevice != INVALID_HANDLE_VALUE)
	{
		return VIGEM_ERROR_BUS_ALREADY_CONNECTED;
	}

	Version latestKnown("0.0.0.0");
	static Version MIN_SUPPORTED_VIGEMBUS = Version("1.16.112.0");
	long checkDeviceIndex = -1;
	DWORD currentIndex = 0;
	DWORD useDeviceIndex = memberIndex;

	const auto deviceInfoSet = SetupDiGetClassDevs(
		&GUID_DEVINTERFACE_BUSENUM_VIGEM,
		nullptr,
		nullptr,
		DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
	);

	SP_DEVINFO_DATA deviceInfoBuffer = { 0 };
	deviceInfoBuffer.cbSize = sizeof(SP_DEVINFO_DATA);

	// enumerate device instances
	while (SetupDiEnumDeviceInfo(
		deviceInfoSet,
		memberIndex++,
		&deviceInfoBuffer
	))
	{
		DEVPROPTYPE currentPropType;
		DWORD propRequiredSize = 0;
		SetupDiGetDeviceProperty(deviceInfoSet,
			&deviceInfoBuffer,
			&DEVPKEY_Device_DriverVersion,
			&currentPropType,
			nullptr,
			0,
			&propRequiredSize,
			0);

		if (propRequiredSize > 0)
		{
			PWCHAR tempPropBuffer = new WCHAR[propRequiredSize];
			if (!tempPropBuffer) continue;

			SetupDiGetDeviceProperty(deviceInfoSet,
				&deviceInfoBuffer,
				&DEVPKEY_Device_DriverVersion,
				&currentPropType,
				(BYTE*)tempPropBuffer,
				propRequiredSize,
				nullptr,
				0);

			char* szTo = new char[propRequiredSize];
			if (!szTo)
			{
				delete[] tempPropBuffer;
				continue;
			}

			WideCharToMultiByte(CP_UTF8, 0, tempPropBuffer, -1, szTo, propRequiredSize, nullptr, nullptr);
			std::string versionString(szTo);
			Version tempVersion(versionString);

			if (latestKnown.CompareTo(tempVersion) < 0)
			{
				latestKnown = tempVersion;
				useDeviceIndex = currentIndex;
				checkDeviceIndex = useDeviceIndex;
			}

			delete[] tempPropBuffer;
			delete[] szTo;
		}

		currentIndex++;
	}

	if (checkDeviceIndex == -1 || latestKnown.CompareTo(MIN_SUPPORTED_VIGEMBUS) < 0)
	{
		SetupDiDestroyDeviceInfoList(deviceInfoSet);
		error = VIGEM_ERROR_BUS_VERSION_MISMATCH;
		return error;
	}

	// only look at one device instance
	bool lookupRan = false;
	while (!lookupRan && SetupDiEnumDeviceInterfaces(
		deviceInfoSet,
		nullptr,
		&GUID_DEVINTERFACE_BUSENUM_VIGEM,
		useDeviceIndex,
		&deviceInterfaceData
	))
	{
		lookupRan = true;

		// get required target buffer size
		SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, nullptr, 0, &requiredSize, nullptr);

		// allocate target buffer
		const auto detailDataBuffer = static_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(malloc(requiredSize));
		detailDataBuffer->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		// get detail buffer
		if (!SetupDiGetDeviceInterfaceDetail(
			deviceInfoSet,
			&deviceInterfaceData,
			detailDataBuffer,
			requiredSize,
			&requiredSize,
			nullptr
		))
		{
			free(detailDataBuffer);
			error = VIGEM_ERROR_BUS_NOT_FOUND;
			continue;
		}

		// bus found, open it
		vigem->hBusDevice = CreateFile(
			detailDataBuffer->DevicePath,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED,
			nullptr
		);

		// check bus open result
		if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		{
			error = VIGEM_ERROR_BUS_ACCESS_FAILED;
			free(detailDataBuffer);
			continue;
		}

		bool versionCheckPassed = false;
		for (int versionAttempt = 0; versionAttempt < CONNECT_VERSION_CHECK_TRIES && !versionCheckPassed; versionAttempt++)
		{
			DWORD transferred = 0;
			OVERLAPPED lOverlapped = { 0 };
			lOverlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

			VIGEM_CHECK_VERSION version;
			VIGEM_CHECK_VERSION_INIT(&version, VIGEM_COMMON_VERSION);

			// send compiled library version to driver to check compatibility
			DeviceIoControl(
				vigem->hBusDevice,
				IOCTL_VIGEM_CHECK_VERSION,
				&version,
				version.Size,
				nullptr,
				0,
				&transferred,
				&lOverlapped
			);

			// wait for result
			if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) != 0)
			{
				vigem->hDS4OutputReportPickupThread = CreateThread(
					nullptr,
					0,
					vigem_internal_ds4_output_report_pickup_handler,
					vigem,
					0,
					nullptr
				);

				error = VIGEM_ERROR_NONE;
				versionCheckPassed = true;
				CloseHandle(lOverlapped.hEvent);
				break;
			}

			const DWORD versionCheckErr = GetLastError();
			CloseHandle(lOverlapped.hEvent);

			// On system resume the driver may not be ready yet; retry transient errors
			if ((versionCheckErr == ERROR_NOT_READY
				|| versionCheckErr == ERROR_IO_DEVICE
				|| versionCheckErr == ERROR_BAD_COMMAND)
				&& versionAttempt < CONNECT_VERSION_CHECK_TRIES - 1)
			{
				Sleep(CONNECT_VERSION_CHECK_DELAY_MS);
				continue;
			}

			// Non-retryable failure
			error = VIGEM_ERROR_BUS_VERSION_MISMATCH;
			break;
		}

		if (!versionCheckPassed)
		{
			free(detailDataBuffer);
			continue;
		}

		free(detailDataBuffer);
	}

	SetupDiDestroyDeviceInfoList(deviceInfoSet);

	return error;
}

void vigem_disconnect(PVIGEM_CLIENT vigem)
{
	if (!vigem)
		return;

	if (vigem->hDS4OutputReportPickupThread && vigem->hDS4OutputReportPickupThreadAbortEvent)
	{
		DBGPRINT(L"Awaiting DS4 thread clean-up for 0x%p", vigem);

		SetEvent(vigem->hDS4OutputReportPickupThreadAbortEvent);

		if (vigem->hBusDevice != INVALID_HANDLE_VALUE)
		{
			DBGPRINT(L"Cancelling all I/O for 0x%p", vigem);
			CancelIoEx(vigem->hBusDevice, nullptr);
		}

		WaitForSingleObject(vigem->hDS4OutputReportPickupThread, INFINITE);
		CloseHandle(vigem->hDS4OutputReportPickupThread);
		CloseHandle(vigem->hDS4OutputReportPickupThreadAbortEvent);

		DBGPRINT(L"DS4 thread clean-up for 0x%p finished", vigem);
	}

	if (vigem->hBusDevice != INVALID_HANDLE_VALUE)
	{
		DBGPRINT(L"Closing bus handle for 0x%p", vigem);

		CloseHandle(vigem->hBusDevice);
		vigem->hBusDevice = INVALID_HANDLE_VALUE;
	}

	RtlZeroMemory(vigem, sizeof(VIGEM_CLIENT));
}

BOOLEAN vigem_target_is_waitable_add_supported(PVIGEM_TARGET target)
{
	//
	// Safety check to make people use the older functions and not cause issues
	// Should never pass in an invalid target but doesn't hurt to check.
	//
	if (!target)
		return FALSE;

	// TODO: Replace all this with a more robust version check system
	return static_cast<BOOLEAN>(!target->IsWaitReadyUnsupported);
}

PVIGEM_TARGET vigem_target_x360_alloc(void)
{
	const auto target = VIGEM_TARGET_ALLOC_INIT(Xbox360Wired);

	if (!target)
		return nullptr;

	target->VendorId = 0x045E;
	target->ProductId = 0x028E;

	return target;
}

PVIGEM_TARGET vigem_target_ds4_alloc(void)
{
	const auto target = VIGEM_TARGET_ALLOC_INIT(DualShock4Wired);

	if (!target)
		return nullptr;

	target->VendorId = 0x054C;
	target->ProductId = 0x05C4;
	target->Ds4CachedOutputReportUpdateAvailable = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	return target;
}

void vigem_target_free(PVIGEM_TARGET target)
{
	if (target)
		free(target);
}

#define WAIT_DEVICE_READY_TRIES 4
#define WAIT_DEVICE_READY_DELAY_MS 1000

#define REMOVE_DEVICE_TRIES 4
#define REMOVE_DEVICE_RETRY_DELAY_MS 1000

VIGEM_ERROR vigem_target_add(PVIGEM_CLIENT vigem, PVIGEM_TARGET target)
{
	VIGEM_ERROR error = VIGEM_ERROR_NO_FREE_SLOT;
	DWORD transferred = 0;
	VIGEM_PLUGIN_TARGET plugin;
	VIGEM_WAIT_DEVICE_READY devReady;
	OVERLAPPED olPlugIn = { 0 };
	olPlugIn.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	OVERLAPPED olWait = { 0 };
	olWait.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	do
	{
		if (!vigem)
		{
			error = VIGEM_ERROR_BUS_INVALID_HANDLE;
			break;
		}

		if (!target)
		{
			error = VIGEM_ERROR_INVALID_TARGET;
			break;
		}

		if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		{
			error = VIGEM_ERROR_BUS_NOT_FOUND;
			break;
		}

		if (target->State == VIGEM_TARGET_NEW)
		{
			error = VIGEM_ERROR_TARGET_UNINITIALIZED;
			break;
		}

		if (target->State == VIGEM_TARGET_CONNECTED)
		{
			error = VIGEM_ERROR_ALREADY_CONNECTED;
			break;
		}

		//
		// TODO: this is mad stupid, redesign, so that the bus fills the assigned slot
		// 
		for (target->SerialNo = 1; target->SerialNo <= VIGEM_TARGETS_MAX; target->SerialNo++)
		{
			// Reset the plugin overlapped event between slot attempts to avoid stale state
			if (olPlugIn.hEvent)
				ResetEvent(olPlugIn.hEvent);

			VIGEM_PLUGIN_TARGET_INIT(&plugin, target->SerialNo, target->Type);

			plugin.VendorId = target->VendorId;
			plugin.ProductId = target->ProductId;

			/*
			 * Request plugin of device. This is an inherently asynchronous operation,
			 * which is addressed differently through the history of the driver design.
			 * Pre-v1.17 this request was kept pending until the child was deemed operational
			 * which unfortunately causes synchronization issues on some systems.
			 * Starting with v1.17 "waiting" for full power-up is done with an additional
			 * IOCTL that is sent immediately after and kept pending until the driver
			 * reports that the device can receive report updates. The following section
			 * and error handling is designed to achieve transparent backwards compatibility
			 * to not break applications using the pre-v1.17 client SDK. This is not a 100%
			 * perfect and can cause other functions to fail if called too soon but
			 * hopefully the applications will just ignore these errors and retry ;)
			 */
			DeviceIoControl(
				vigem->hBusDevice,
				IOCTL_VIGEM_PLUGIN_TARGET,
				&plugin,
				plugin.Size,
				nullptr,
				0,
				&transferred,
				&olPlugIn
			);

			//
			// This should return fairly immediately >=v1.17
			// 
			if (GetOverlappedResult(vigem->hBusDevice, &olPlugIn, &transferred, TRUE) != 0)
			{
				bool waitSuccess = false;
				bool hardwareErrorExhausted = false;
				for (int i = 0; i < WAIT_DEVICE_READY_TRIES && !waitSuccess; i++)
				{
					/*
					* This function is announced to be blocking/synchronous, a concept that
					* doesn't reflect the way the bus driver/PNP manager bring child devices
					* to life. Therefore, we send another IOCTL which will be kept pending
					* until the bus driver has been notified that the child device has
					* reached a state that is deemed operational. This request is only
					* supported on drivers v1.17 or higher, so gracefully cause errors
					* of this call as a potential success and keep the device plugged in.
					*/
					VIGEM_WAIT_DEVICE_READY_INIT(&devReady, plugin.SerialNo);

					DeviceIoControl(
						vigem->hBusDevice,
						IOCTL_VIGEM_WAIT_DEVICE_READY,
						&devReady,
						devReady.Size,
						nullptr,
						0,
						&transferred,
						&olWait
					);

					bool lastRun = i == WAIT_DEVICE_READY_TRIES - 1;
					if (GetOverlappedResult(vigem->hBusDevice, &olWait, &transferred, TRUE) != 0)
					{
						target->State = VIGEM_TARGET_CONNECTED;

						error = VIGEM_ERROR_NONE;
						waitSuccess = true;
						break;
					}

					DWORD lastError = GetLastError();
					//
					// Backwards compatibility with version pre-1.17, where this IOCTL doesn't exist
					//
					if (lastError == ERROR_INVALID_PARAMETER)
					{
						target->State = VIGEM_TARGET_CONNECTED;
						target->IsWaitReadyUnsupported = true;

						error = VIGEM_ERROR_NONE;
						waitSuccess = true;
						break;
					}
					else if (lastError != ERROR_DEVICE_HARDWARE_ERROR)
					{
						// Plugin failure occurred. Ignore ERROR_DEVICE_HARDWARE_ERROR as that
						// indicates a timeout looking for an XUSB LED packet. Try again
						// for that case.
						break;
					}

					if (!lastRun)
					{
						Sleep(WAIT_DEVICE_READY_DELAY_MS);

						HANDLE waitEvent = olWait.hEvent;
						memset(&olWait, 0, sizeof(olWait));
						if (waitEvent)
						{
							ResetEvent(waitEvent);
							olWait.hEvent = waitEvent;
						}
					}
					else
					{
						// All inner wait attempts were consumed exclusively with
						// ERROR_DEVICE_HARDWARE_ERROR. This is the signature of the USB/HID
						// stack still reinitializing after a system resume from sleep or
						// hibernation. Surface a dedicated error so the caller can implement
						// its own retry policy (e.g. after receiving WM_POWERBROADCAST /
						// PBT_APMRESUMEAUTOMATIC) rather than sleeping inside the library.
						hardwareErrorExhausted = true;
					}
				}

				if (waitSuccess)
				{
					break;
				}

				//
				// Don't leave device connected if the wait call failed
				//
				vigem_target_remove(vigem, target);

				//
				// Surface a dedicated error when the USB/HID stack was still reinitializing
				// (all wait attempts failed with ERROR_DEVICE_HARDWARE_ERROR). The caller
				// can detect VIGEM_ERROR_DEVICE_NOT_READY and schedule its own retry,
				// for example after receiving WM_POWERBROADCAST / PBT_APMRESUMEAUTOMATIC.
				//
				error = hardwareErrorExhausted
					? VIGEM_ERROR_DEVICE_NOT_READY
					: VIGEM_ERROR_NO_FREE_SLOT;
				break;
			}
		}
	} while (false);

	if (VIGEM_SUCCESS(error))
	{
		vigem->pTargetsList[target->SerialNo] = target;
	}

	if (olPlugIn.hEvent)
		CloseHandle(olPlugIn.hEvent);

	if (olWait.hEvent)
		CloseHandle(olWait.hEvent);

	return error;
}

VIGEM_ERROR vigem_target_add_async(PVIGEM_CLIENT vigem, PVIGEM_TARGET target, PFN_VIGEM_TARGET_ADD_RESULT result)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->State == VIGEM_TARGET_NEW)
		return VIGEM_ERROR_TARGET_UNINITIALIZED;

	if (target->State == VIGEM_TARGET_CONNECTED)
		return VIGEM_ERROR_ALREADY_CONNECTED;

	std::thread _async{
		[](
		PVIGEM_TARGET _Target,
		PVIGEM_CLIENT _Client,
		PFN_VIGEM_TARGET_ADD_RESULT _Result)
		{
			const auto error = vigem_target_add(_Client, _Target);

			_Result(_Client, _Target, error);
		},
		target, vigem, result
	};

	_async.detach();

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_remove(PVIGEM_CLIENT vigem, PVIGEM_TARGET target)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->State == VIGEM_TARGET_NEW)
		return VIGEM_ERROR_TARGET_UNINITIALIZED;

	if (target->State != VIGEM_TARGET_CONNECTED)
		return VIGEM_ERROR_TARGET_NOT_PLUGGED_IN;

	VIGEM_UNPLUG_TARGET unplug;
	VIGEM_UNPLUG_TARGET_INIT(&unplug, target->SerialNo);

	VIGEM_ERROR result = VIGEM_ERROR_REMOVAL_FAILED;

	// We re-create OVERLAPPED per try to avoid stale signaled events between attempts
	for (int attempt = 0; attempt < REMOVE_DEVICE_TRIES; ++attempt)
	{
		DEVICE_IO_CONTROL_BEGIN;

		DeviceIoControl(
			vigem->hBusDevice,
			IOCTL_VIGEM_UNPLUG_TARGET,
			&unplug,
			unplug.Size,
			nullptr,
			0,
			&transferred,
			&lOverlapped
		);

		if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) != 0)
		{
			// Success path: clean up and mark disconnected
			if (target->Ds4CachedOutputReportUpdateAvailable)
			{
				CloseHandle(target->Ds4CachedOutputReportUpdateAvailable);
				target->Ds4CachedOutputReportUpdateAvailable = nullptr;
			}

			vigem->pTargetsList[target->SerialNo] = nullptr;
			target->State = VIGEM_TARGET_DISCONNECTED;

			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_NONE;
		}

		// Failure: decide whether to retry or bail out
		const DWORD err = GetLastError();

		// Treat these as transient or "already gone" conditions; retry or accept as success where appropriate
		if (err == ERROR_DEVICE_NOT_CONNECTED)
		{
			// If the device is already considered gone by the bus, mark as removed locally
			if (target->Ds4CachedOutputReportUpdateAvailable)
			{
				CloseHandle(target->Ds4CachedOutputReportUpdateAvailable);
				target->Ds4CachedOutputReportUpdateAvailable = nullptr;
			}
			vigem->pTargetsList[target->SerialNo] = nullptr;
			target->State = VIGEM_TARGET_DISCONNECTED;

			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_NONE;
		}

		// Access denied/operation aborted can happen during teardown/cancel; retry a couple of times
		if ((err == ERROR_ACCESS_DENIED) || (err == ERROR_OPERATION_ABORTED) || (err == ERROR_RETRY))
		{
			DEVICE_IO_CONTROL_END;

			if (attempt < (REMOVE_DEVICE_TRIES - 1))
			{
				Sleep(REMOVE_DEVICE_RETRY_DELAY_MS);
				continue; // try again
			}
			else
			{
				// Out of tries
				break;
			}
		}

		// Any other error: break and return failure
		DEVICE_IO_CONTROL_END;
		break;
	}

	return result;
}

// Num of items in Notification DeviceIOControl queue (at any time there should be at least one extra call waiting for the new events or there is danger that notification events are lost).
// The size of this queue is based on "scientific" experimentals and estimations (few games seem to sometimes flood the FFB driver interface).
// Keeping the size of 6 for now. Based on previous tests with ParaLLEl64 core in Retroarch, the minimum should be 4.
#define NOTIFICATION_OVERLAPPED_QUEUE_SIZE 6

VIGEM_ERROR vigem_target_x360_register_notification(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	PFN_VIGEM_X360_NOTIFICATION notification,
	LPVOID userData
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0 || notification == nullptr)
		return VIGEM_ERROR_INVALID_TARGET;

	if (target->Notification == reinterpret_cast<FARPROC>(notification))
		return VIGEM_ERROR_CALLBACK_ALREADY_REGISTERED;

	target->Notification = reinterpret_cast<FARPROC>(notification);
	target->NotificationUserData = userData;

	if (target->CancelNotificationThreadEvent == nullptr)
		target->CancelNotificationThreadEvent = CreateEvent(
			nullptr,
			TRUE,
			FALSE,
			nullptr
		);
	else
		ResetEvent(target->CancelNotificationThreadEvent);

	std::thread _async{
		[](
		PVIGEM_TARGET _Target,
		PVIGEM_CLIENT _Client,
		LPVOID _UserData)
		{
			DWORD transferred[NOTIFICATION_OVERLAPPED_QUEUE_SIZE] = { };
			OVERLAPPED lOverlapped[NOTIFICATION_OVERLAPPED_QUEUE_SIZE] = { };
			std::unique_ptr<NotificationRequestPayloadX360> payloads[NOTIFICATION_OVERLAPPED_QUEUE_SIZE] = { };

			memset(transferred, 0, sizeof(transferred));
			memset(lOverlapped, 0, sizeof(lOverlapped));

			for (int idx = 0; idx < NOTIFICATION_OVERLAPPED_QUEUE_SIZE; idx++)
				payloads[idx] = std::unique_ptr<NotificationRequestPayloadX360>(new NotificationRequestPayloadX360(_Target->SerialNo));

			for (int idx = 0; idx < NOTIFICATION_OVERLAPPED_QUEUE_SIZE; idx++)
				lOverlapped[idx].hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

			int currentOverlappedIdx = 0;
			int futureOverlappedIdx = NOTIFICATION_OVERLAPPED_QUEUE_SIZE - 1;

			// Send out DeviceIOControl calls to wait for incoming feedback notifications. Use N pending requests to make sure that events are not lost even when application would flood FFB events.
			// The order of DeviceIoControl calls and GetOverlappedResult requests is important to ensure that feedback callback function is called in correct order (ie. FIFO buffer with FFB events).
			// Note! This loop doesn't call DevIo for the last lOverlapped item on purpose. The DO while loop does it as a first step (futureOverlappedIdx=NOTIFICATION_OVERLAPPED_QUEUE_SIZE-1 in the first loop round).
			for (int idx = 0; idx < NOTIFICATION_OVERLAPPED_QUEUE_SIZE - 1; idx++)
			{
				NotificationRequestPayloadX360* current = payloads[idx].get();
				DeviceIoControl(_Client->hBusDevice,
					current->ioControlCode,
					current->lpPayloadBuffer,
					current->payloadBufferSize,
					current->lpPayloadBuffer,
					current->payloadBufferSize,
					&transferred[idx],
					&lOverlapped[idx]);
			}

			do
			{
				NotificationRequestPayloadX360* futureNotify = payloads[futureOverlappedIdx].get();
				DeviceIoControl(
					_Client->hBusDevice,
					futureNotify->ioControlCode,
					futureNotify->lpPayloadBuffer,
					futureNotify->payloadBufferSize,
					futureNotify->lpPayloadBuffer,
					futureNotify->payloadBufferSize,
					&transferred[futureOverlappedIdx],
					&lOverlapped[futureOverlappedIdx]
				);

				if (GetOverlappedResult(_Client->hBusDevice, &lOverlapped[currentOverlappedIdx], &transferred[currentOverlappedIdx], TRUE) != 0)
				{
					if (_Target->Notification == nullptr)
					{
						for (int idx = 0; idx < NOTIFICATION_OVERLAPPED_QUEUE_SIZE; idx++)
						{
							if (lOverlapped[idx].hEvent)
							{
								CloseHandle(lOverlapped[idx].hEvent);
							}
						}

						return;
					}

					NotificationRequestPayloadX360* currentPayload = payloads[currentOverlappedIdx].get();
					currentPayload->ProcessNotificationRequest(_Client, _Target);

					if (currentOverlappedIdx >= NOTIFICATION_OVERLAPPED_QUEUE_SIZE - 1)
						currentOverlappedIdx = 0;
					else
						currentOverlappedIdx++;

					if (futureOverlappedIdx >= NOTIFICATION_OVERLAPPED_QUEUE_SIZE - 1)
						futureOverlappedIdx = 0;
					else
						futureOverlappedIdx++;

					/*reinterpret_cast<PFN_VIGEM_X360_NOTIFICATION>(_Target->Notification)(
						_Client, _Target, currentNotify->LargeMotor, currentNotify->SmallMotor, currentNotify->LedNumber, _UserData
					);*/

									continue;
								}

								{
									const DWORD lastErr = GetLastError();
									if (lastErr != ERROR_IO_PENDING)
									{
										// Exit on any unrecoverable error (access denied, aborted, invalid handle, device gone, etc.)
										break;
									}
								}
							} while (WaitForSingleObjectEx(_Target->CancelNotificationThreadEvent, 0, FALSE) == WAIT_TIMEOUT);

							for (int idx = 0; idx < NOTIFICATION_OVERLAPPED_QUEUE_SIZE; idx++)
							{
								if (lOverlapped[idx].hEvent)
								{
									CloseHandle(lOverlapped[idx].hEvent);
								}
							}
							},
							target, vigem, userData
	};

	_async.detach();

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_register_notification(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	PFN_VIGEM_DS4_NOTIFICATION notification,
	LPVOID userData
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0 || notification == nullptr)
		return VIGEM_ERROR_INVALID_TARGET;

	if (target->Notification == reinterpret_cast<FARPROC>(notification))
		return VIGEM_ERROR_CALLBACK_ALREADY_REGISTERED;

	target->Notification = reinterpret_cast<FARPROC>(notification);
	target->NotificationUserData = userData;

	if (target->CancelNotificationThreadEvent == nullptr)
		target->CancelNotificationThreadEvent = CreateEvent(
			nullptr,
			TRUE,
			FALSE,
			nullptr
		);
	else
		ResetEvent(target->CancelNotificationThreadEvent);

	std::thread _async{
		[](
		PVIGEM_TARGET _Target,
		PVIGEM_CLIENT _Client,
		LPVOID _UserData)
		{
			DWORD transferred[NOTIFICATION_OVERLAPPED_QUEUE_SIZE] = { };
			OVERLAPPED lOverlapped[NOTIFICATION_OVERLAPPED_QUEUE_SIZE] = { };
			std::unique_ptr<NotificationRequestPayloadDS4> payloads[NOTIFICATION_OVERLAPPED_QUEUE_SIZE] = { };

			memset(transferred, 0, sizeof(transferred));
			memset(lOverlapped, 0, sizeof(lOverlapped));

			for (int idx = 0; idx < NOTIFICATION_OVERLAPPED_QUEUE_SIZE; idx++)
				payloads[idx] = std::unique_ptr<NotificationRequestPayloadDS4>(new NotificationRequestPayloadDS4(_Target->SerialNo));

			for (int idx = 0; idx < NOTIFICATION_OVERLAPPED_QUEUE_SIZE; idx++)
			 lOverlapped[idx].hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

			int currentOverlappedIdx = 0;
			int futureOverlappedIdx = NOTIFICATION_OVERLAPPED_QUEUE_SIZE - 1;

			// Send out DeviceIOControl calls to wait for incoming feedback notifications. Use N pending requests to make sure that events are not lost even when application would flood FFB events.
			// The order of DeviceIoControl calls and GetOverlappedResult requests is important to ensure that feedback callback function is called in correct order (ie. FIFO buffer with FFB events).
			// Note! This loop doesn't call DevIo for the last lOverlapped item on purpose. The DO while loop does it as a first step (futureOverlappedIdx=NOTIFICATION_OVERLAPPED_QUEUE_SIZE-1 in the first loop round).
			for (int idx = 0; idx < NOTIFICATION_OVERLAPPED_QUEUE_SIZE - 1; idx++)
			{
				NotificationRequestPayloadDS4* current = payloads[idx].get();
				DeviceIoControl(_Client->hBusDevice,
					current->ioControlCode,
					current->lpPayloadBuffer,
					current->payloadBufferSize,
					current->lpPayloadBuffer,
					current->payloadBufferSize,
					&transferred[idx],
					&lOverlapped[idx]);
			}

			do
			{
				NotificationRequestPayloadDS4* futureNotify = payloads[futureOverlappedIdx].get();
				DeviceIoControl(
					_Client->hBusDevice,
					futureNotify->ioControlCode,
					futureNotify->lpPayloadBuffer,
					futureNotify->payloadBufferSize,
					futureNotify->lpPayloadBuffer,
					futureNotify->payloadBufferSize,
					&transferred[futureOverlappedIdx],
					&lOverlapped[futureOverlappedIdx]
				);

				if (GetOverlappedResult(_Client->hBusDevice, &lOverlapped[currentOverlappedIdx], &transferred[currentOverlappedIdx], TRUE) != 0)
				{
					if (_Target->Notification == nullptr)
					{
						for (int idx = 0; idx < NOTIFICATION_OVERLAPPED_QUEUE_SIZE; idx++)
						{
							if (lOverlapped[idx].hEvent)
							{
								CloseHandle(lOverlapped[idx].hEvent);
							}
						}

						return;
					}

					NotificationRequestPayloadDS4* currentPayload = payloads[currentOverlappedIdx].get();
					currentPayload->ProcessNotificationRequest(_Client, _Target);

					if (currentOverlappedIdx >= NOTIFICATION_OVERLAPPED_QUEUE_SIZE - 1)
						currentOverlappedIdx = 0;
					else
						currentOverlappedIdx++;

					if (futureOverlappedIdx >= NOTIFICATION_OVERLAPPED_QUEUE_SIZE - 1)
						futureOverlappedIdx = 0;
					else
						futureOverlappedIdx++;

					/*reinterpret_cast<PFN_VIGEM_DS4_NOTIFICATION>(_Target->Notification)(
						_Client, _Target, ds4rn.Report.LargeMotor,
						ds4rn.Report.SmallMotor,
						ds4rn.Report.LightbarColor, _UserData
					);
					*/

									continue;
								}

								{
									const DWORD lastErr = GetLastError();
									if (lastErr != ERROR_IO_PENDING)
									{
										// Exit on any unrecoverable error (access denied, aborted, invalid handle, device gone, etc.)
										break;
									}
								}
							} while (WaitForSingleObjectEx(_Target->CancelNotificationThreadEvent, 0, FALSE) == WAIT_TIMEOUT);

							for (int idx = 0; idx < NOTIFICATION_OVERLAPPED_QUEUE_SIZE; idx++)
							{
								if (lOverlapped[idx].hEvent)
								{
									CloseHandle(lOverlapped[idx].hEvent);
								}
							}
							},
							target, vigem, userData
	};

	_async.detach();

	return VIGEM_ERROR_NONE;
}

void vigem_target_x360_unregister_notification(PVIGEM_TARGET target)
{
	if (target->CancelNotificationThreadEvent != nullptr)
		SetEvent(target->CancelNotificationThreadEvent);

	if (target->CancelNotificationThreadEvent != nullptr)
	{
		CloseHandle(target->CancelNotificationThreadEvent);
		target->CancelNotificationThreadEvent = nullptr;
	}

	target->Notification = nullptr;
	target->NotificationUserData = nullptr;
}

void vigem_target_ds4_unregister_notification(PVIGEM_TARGET target)
{
	vigem_target_x360_unregister_notification(target); // The same x360_unregister handler works for DS4_unregister also
}

void vigem_target_set_vid(PVIGEM_TARGET target, USHORT vid)
{
	target->VendorId = vid;
}

void vigem_target_set_pid(PVIGEM_TARGET target, USHORT pid)
{
	target->ProductId = pid;
}

USHORT vigem_target_get_vid(PVIGEM_TARGET target)
{
	return target->VendorId;
}

USHORT vigem_target_get_pid(PVIGEM_TARGET target)
{
	return target->ProductId;
}

VIGEM_ERROR vigem_target_x360_update(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	XUSB_REPORT report
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0)
		return VIGEM_ERROR_INVALID_TARGET;

	DEVICE_IO_CONTROL_BEGIN;

	XUSB_SUBMIT_REPORT xsr;
	XUSB_SUBMIT_REPORT_INIT(&xsr, target->SerialNo);

	xsr.Report = report;

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_XUSB_SUBMIT_REPORT,
		&xsr,
		xsr.Size,
		nullptr,
		0,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) == 0)
	{
		if (GetLastError() == ERROR_ACCESS_DENIED)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_INVALID_TARGET;
		}
	}

	DEVICE_IO_CONTROL_END;

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_update(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	DS4_REPORT report
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0)
		return VIGEM_ERROR_INVALID_TARGET;

	DEVICE_IO_CONTROL_BEGIN;

	DS4_SUBMIT_REPORT dsr;
	DS4_SUBMIT_REPORT_INIT(&dsr, target->SerialNo);

	dsr.Report = report;

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_DS4_SUBMIT_REPORT,
		&dsr,
		dsr.Size,
		nullptr,
		0,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) == 0)
	{
		if (GetLastError() == ERROR_ACCESS_DENIED)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_INVALID_TARGET;
		}
	}

	DEVICE_IO_CONTROL_END;

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_update_ex(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	DS4_REPORT_EX report
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0)
		return VIGEM_ERROR_INVALID_TARGET;

	DEVICE_IO_CONTROL_BEGIN;

	DS4_SUBMIT_REPORT_EX dsr;
	DS4_SUBMIT_REPORT_EX_INIT(&dsr, target->SerialNo);

	dsr.Report = report;

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_DS4_SUBMIT_REPORT, // Same IOCTL, just different size
		&dsr,
		dsr.Size,
		nullptr,
		0,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) == 0)
	{
		if (GetLastError() == ERROR_ACCESS_DENIED)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_INVALID_TARGET;
		}

		/*
		 * NOTE: this will not happen on v1.16 due to NTSTATUS accidentally been set
		 * as STATUS_SUCCESS when the submitted buffer size wasn't the expected one.
		 * For backwards compatibility this function will silently fail (not cause
		 * report updates) when run with the v1.16 driver. This API was introduced
		 * with v1.17 so it won't affect existing applications built before.
		 */
		if (GetLastError() == ERROR_INVALID_PARAMETER)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_NOT_SUPPORTED;
		}
	}

	DEVICE_IO_CONTROL_END;

	return VIGEM_ERROR_NONE;
}

ULONG vigem_target_get_index(PVIGEM_TARGET target)
{
	return target->SerialNo;
}

VIGEM_TARGET_TYPE vigem_target_get_type(PVIGEM_TARGET target)
{
	return target->Type;
}

BOOL vigem_target_is_attached(PVIGEM_TARGET target)
{
	return (target->State == VIGEM_TARGET_CONNECTED);
}

VIGEM_ERROR vigem_target_x360_get_user_index(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	PULONG index
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0 || target->Type != Xbox360Wired)
		return VIGEM_ERROR_INVALID_TARGET;

	if (!index)
		return VIGEM_ERROR_INVALID_PARAMETER;

	DEVICE_IO_CONTROL_BEGIN;

	XUSB_GET_USER_INDEX gui;
	XUSB_GET_USER_INDEX_INIT(&gui, target->SerialNo);

	DeviceIoControl(
		vigem->hBusDevice,
		IOCTL_XUSB_GET_USER_INDEX,
		&gui,
		gui.Size,
		&gui,
		gui.Size,
		&transferred,
		&lOverlapped
	);

	if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transferred, TRUE) == 0)
	{
		const auto error = GetLastError();

		if (error == ERROR_ACCESS_DENIED)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_INVALID_TARGET;
		}

		if (error == ERROR_INVALID_DEVICE_OBJECT_PARAMETER)
		{
			DEVICE_IO_CONTROL_END;
			return VIGEM_ERROR_XUSB_USERINDEX_OUT_OF_RANGE;
		}
	}

	DEVICE_IO_CONTROL_END;

	*index = gui.UserIndex;

	return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_await_output_report(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	PDS4_OUTPUT_BUFFER buffer
)
{
	return vigem_target_ds4_await_output_report_timeout(vigem, target, INFINITE, buffer);
}

VIGEM_ERROR vigem_target_ds4_await_output_report_timeout(
	PVIGEM_CLIENT vigem,
	PVIGEM_TARGET target,
	DWORD milliseconds,
	PDS4_OUTPUT_BUFFER buffer
)
{
	if (!vigem)
		return VIGEM_ERROR_BUS_INVALID_HANDLE;

	if (!target)
		return VIGEM_ERROR_INVALID_TARGET;

	if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
		return VIGEM_ERROR_BUS_NOT_FOUND;

	if (target->SerialNo == 0)
		return VIGEM_ERROR_INVALID_TARGET;

	if (!buffer)
		return VIGEM_ERROR_INVALID_PARAMETER;

	const DWORD status = WaitForSingleObject(target->Ds4CachedOutputReportUpdateAvailable, milliseconds);

	if (status == WAIT_TIMEOUT)
	{
		return VIGEM_ERROR_TIMED_OUT;
	}

#if defined(VIGEM_VERBOSE_LOGGING_ENABLED)
	DBGPRINT(L"Dumping buffer for %d", target->SerialNo);

	const PCHAR dumpBuffer = (PCHAR)calloc(sizeof(DS4_OUTPUT_BUFFER), 3);
	to_hex(target->Ds4CachedOutputReport.Buffer, sizeof(DS4_OUTPUT_BUFFER), dumpBuffer, sizeof(DS4_OUTPUT_BUFFER) * 3);
	OutputDebugStringA(dumpBuffer);
#endif

	RtlCopyMemory(buffer, &target->Ds4CachedOutputReport, sizeof(DS4_OUTPUT_BUFFER));

	return VIGEM_ERROR_NONE;
}
