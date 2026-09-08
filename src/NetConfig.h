// NetConfig.h -- adapter enumeration and IPv4 configuration, with no UI in it.
//
// Reading is done with the IP Helper API (fast, no COM). Writing is done with
// WMI's Win32_NetworkAdapterConfiguration, because EnableStatic() takes arrays
// of addresses and masks -- which is exactly the "additional IP/mask" case --
// and because it persists the same way the control panel dialogs do.
#pragma once

#include <string>
#include <vector>

namespace net {

// Duplicate address detection can reject an address that is still reported by
// GetAdaptersAddresses. ipconfig flags or hides those, so ignoring this is how
// the dialog and ipconfig come to disagree about the same adapter.
enum class AddressState { Preferred, Tentative, Duplicate, Deprecated, Invalid };

const wchar_t* stateText(AddressState state);

struct AddressV4 {
    std::wstring ip;
    std::wstring mask;
    AddressState state = AddressState::Preferred;
    bool manual = true;                 // false when DHCP or autoconfiguration set it
};

struct AdapterInfo {
    std::wstring friendlyName;          // "Ethernet 2"
    std::wstring description;           // driver description
    std::wstring settingId;             // "{GUID}" -- matches WMI SettingID
    std::wstring macAddress;            // "00-11-22-33-44-55", empty if none
    unsigned long ifIndex = 0;
    bool dhcpEnabled = false;
    bool operational = false;           // IfOperStatusUp
    std::vector<AddressV4> addresses;   // [0] is the primary, if any
    std::vector<std::wstring> gateways;
    std::vector<std::wstring> dnsServers;
};

// Returns the IPv4-capable adapters, loopback excluded. On failure the vector
// is empty and `error` says why.
std::vector<AdapterInfo> enumerateAdapters(std::wstring& error);

struct ApplyRequest {
    std::wstring settingId;
    bool useDhcp = false;
    std::vector<AddressV4> addresses;   // static only; [0] is the primary
    std::wstring gateway;               // static only; blank clears the gateway
    std::vector<std::wstring> dnsServers;
};

struct ApplyResult {
    bool ok = false;
    bool rebootRequired = false;
    std::wstring message;
};

// Applies `req`. Blocking, and can take several seconds -- call it off the UI
// thread. It initialises and uninitialises COM on the calling thread itself,
// so the caller need not.
ApplyResult apply(const ApplyRequest& req);

}  // namespace net
