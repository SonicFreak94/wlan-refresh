#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <wlanapi.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace std::chrono;

// above-zero codes are non-critical;
// below-zero is critical
enum class error_codes : int
{
	none                       =  0,
	interface_scan_failed      =  1,
	wlan_open_failed           = -1,
	interface_enum_failed      = -2,
	no_interface               = -3,
	all_interface_scans_failed = -4
};

static GUID scan_guid {};
static std::atomic<bool> scan_complete = false;

// this callback will be used to wait for WlanScan to complete.
static void wlan_callback(PWLAN_NOTIFICATION_DATA Arg1, PVOID Arg2)
{
	// might be worth handling wlan_notification_acm_scan_fail
	if (Arg1->InterfaceGuid == scan_guid &&
	    Arg1->NotificationCode == wlan_notification_acm_scan_complete)
	{
		scan_complete = true;
	}
}

int main(int argc, const char** argv)
{
	bool output_networks = false;
	bool include_connected = false;

	for (int i = 1; i < argc; ++i)
	{
		auto arg = argv[i];

		if (!strcmp(arg, "--list") || !strcmp(arg, "-l"))
		{
			output_networks = true;
		}

		if (!strcmp(arg, "--include-connected") || !strcmp(arg, "-i"))
		{
			include_connected = true;
		}

		if (!strcmp(arg, "--help") || !strcmp(arg, "-h") || !strcmp(arg, "-?"))
		{
		#define ERROR_PLEASE(A) "\t" # A << ": " << static_cast<int>(error_codes:: ## A)

			// this is why printf is better
			std::cout
				<< "Utility for requesting immediate refresh of available Wi-Fi networks." << std::endl
				<< "Parameters:" << std::endl
				<< "\t-l, --list               Output the list of available networks to stdout" << std::endl
				<< "\t-i, --include-connected  Include currently connected networks" << std::endl
				<< std::endl
				<< "Error codes:" << std::endl
				<< "\tPositive error codes are non-critical (warnings)." << std::endl << std::endl
				<< ERROR_PLEASE(none) << std::endl
				<< ERROR_PLEASE(interface_scan_failed) << std::endl
				<< ERROR_PLEASE(wlan_open_failed) << std::endl
				<< ERROR_PLEASE(interface_enum_failed) << std::endl
				<< ERROR_PLEASE(no_interface) << std::endl
				<< ERROR_PLEASE(all_interface_scans_failed) << std::endl;

			return 0;
		}
	}

	DWORD negotiated_version = 0;
	HANDLE handle = nullptr;

	HRESULT hr = WlanOpenHandle(WLAN_API_VERSION, nullptr, &negotiated_version, &handle);

	if (FAILED(hr))
	{
		std::cout << "WlanOpenHandle failed with error code: " << hr << std::endl;
		return static_cast<int>(error_codes::wlan_open_failed);
	}

	// this registers a callback that allows us to wait for WlanScan to complete.
	// it will automatically unregister when WlanCloseHandle is called, but can
	// otherwise be unregistered with WLAN_NOTIFICATION_SOURCE_NONE.
	hr = WlanRegisterNotification(handle, WLAN_NOTIFICATION_SOURCE_ACM, true,
	                              &wlan_callback, nullptr, nullptr, 0);

	if (FAILED(hr))
	{
		std::cerr << "Warning: WlanRegisterNotification failed. Scan will time out waiting for completion." << std::endl;
	}

	PWLAN_INTERFACE_INFO_LIST interface_list = nullptr;
	hr = WlanEnumInterfaces(handle, nullptr, &interface_list);

	if (FAILED(hr))
	{
		WlanCloseHandle(handle, nullptr);
		std::cerr << "WlanEnumInterfaces failed with error code: " << hr << std::endl;
		return static_cast<int>(error_codes::interface_enum_failed);
	}

	if (interface_list->dwNumberOfItems == 0)
	{
		WlanFreeMemory(interface_list);
		WlanCloseHandle(handle, nullptr);
		std::cerr << "WlanEnumInterfaces returned zero interfaces!" << std::endl;
		return static_cast<int>(error_codes::no_interface);
	}

	DWORD interface_count = interface_list->dwNumberOfItems;
	DWORD interface_scan_failures = 0;

	for (size_t i = 0; i < interface_list->dwNumberOfItems; ++i)
	{
		const auto& info = interface_list->InterfaceInfo[i];

		const GUID& guid = info.InterfaceGuid;
		scan_guid = guid;

		// this is used to detect the completion of the WlanScan callback
		scan_complete = false;

		// this requests a refresh on the list of detected wifi networks
		hr = WlanScan(handle, &guid, nullptr, nullptr, nullptr);

		if (SUCCEEDED(hr))
		{
			constexpr auto now = []() -> auto
			{
				return high_resolution_clock::now();
			};

			// microsoft recommends waiting for (and requires that drivers only take)
			// four seconds to complete the scan, so we're gonna bail after that if
			// we don't hear back from the callback.
			const auto start = now();

			while (!scan_complete && now() - start < 4s)
			{
				std::this_thread::sleep_for(1ms);
			}
		}

		if (!output_networks)
		{
			continue;
		}

		// note that WlanGetAvailableNetworkList2 (*2*) does not exist on Windows 7
		// (not that we needed the info provided by it anyway, but LET IT BE KNOWN)
		PWLAN_AVAILABLE_NETWORK_LIST available_networks = nullptr;
		hr = WlanGetAvailableNetworkList(handle, &guid, 0, nullptr, &available_networks);

		if (FAILED(hr))
		{
			++interface_scan_failures;
		}
		else
		{
			for (size_t n = 0; n < available_networks->dwNumberOfItems; ++n)
			{
				const auto& network = available_networks->Network[n];

				if (network.strProfileName[0] != L'\0' && !include_connected)
				{
					continue;
				}

				std::cout << network.dot11Ssid.ucSSID << std::endl;
			}
		}

		WlanFreeMemory(available_networks);
	}

	WlanFreeMemory(interface_list);
	interface_list = nullptr;

	WlanCloseHandle(handle, nullptr);

	if (interface_scan_failures)
	{
		if (interface_scan_failures == interface_count)
		{
			// critical error
			return static_cast<int>(error_codes::all_interface_scans_failed);
		}
		else
		{
			// pretty much just a warning
			return static_cast<int>(error_codes::interface_scan_failed);
		}
	}

	return static_cast<int>(error_codes::none);
}
