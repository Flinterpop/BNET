#include "NetConfig.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <wbemidl.h>

#include <cstdio>

namespace net {
namespace {

// ---------------------------------------------------------------- small RAII

class Bstr {
public:
    explicit Bstr(const wchar_t* s) : b_(SysAllocString(s)) {}
    ~Bstr() { if (b_) SysFreeString(b_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return b_; }          // NOLINT: intentional
    bool valid() const { return b_ != nullptr; }
private:
    BSTR b_ = nullptr;
};

template <typename T>
void release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

std::wstring hresultText(HRESULT hr) {
    wchar_t buf[64];
    swprintf_s(buf, L"0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// ------------------------------------------------------------- enumeration

std::wstring toString(const sockaddr* sa) {
    if (!sa || sa->sa_family != AF_INET) return {};
    const sockaddr_in* in4 = reinterpret_cast<const sockaddr_in*>(sa);
    wchar_t buf[INET_ADDRSTRLEN] = {};
    if (!InetNtopW(AF_INET, &in4->sin_addr, buf, INET_ADDRSTRLEN)) return {};
    return buf;
}

std::wstring maskFromPrefix(UINT8 prefixLength) {
    ULONG mask = 0;
    if (ConvertLengthToIpv4Mask(prefixLength, &mask) != NO_ERROR) return {};
    IN_ADDR a;
    a.S_un.S_addr = mask;
    wchar_t buf[INET_ADDRSTRLEN] = {};
    if (!InetNtopW(AF_INET, &a, buf, INET_ADDRSTRLEN)) return {};
    return buf;
}

std::wstring formatMac(const BYTE* bytes, ULONG length) {
    if (length == 0 || length > 8) return {};
    std::wstring out;
    for (ULONG i = 0; i < length; ++i) {
        wchar_t buf[4];
        swprintf_s(buf, L"%02X", bytes[i]);
        if (i) out += L'-';
        out += buf;
    }
    return out;
}

// ------------------------------------------------------------------- WMI

// Every code Win32_NetworkAdapterConfiguration is documented to return that a
// user of this dialog can actually provoke. Anything else falls through to the
// bare number, which is still better than "it failed".
std::wstring wmiReturnText(unsigned long rc) {
    switch (rc) {
        case 0:  return L"Success.";
        case 1:  return L"Success. A restart is required to take effect.";
        case 64: return L"Method not supported on this platform.";
        case 65: return L"Unknown failure.";
        case 66: return L"Invalid subnet mask.";
        case 67: return L"An error occurred while processing an instance that was returned.";
        case 68: return L"Invalid input parameter.";
        case 69: return L"More than five gateways specified.";
        case 70: return L"Invalid IP address.";
        case 71: return L"Invalid gateway IP address.";
        case 72: return L"An error occurred while accessing the registry for the requested information.";
        case 73: return L"Invalid domain name.";
        case 74: return L"Invalid host name.";
        case 75: return L"No primary or secondary WINS server defined.";
        case 76: return L"Invalid file.";
        case 77: return L"Invalid system path.";
        case 78: return L"File copy failed.";
        case 79: return L"Invalid security parameter.";
        case 80: return L"Unable to configure TCP/IP service.";
        case 81: return L"Unable to configure DHCP service.";
        case 82: return L"Unable to renew the DHCP lease.";
        case 83: return L"Unable to release the DHCP lease.";
        case 84: return L"IP not enabled on this adapter.";
        case 91: return L"Access denied. Run this program elevated.";
        case 92: return L"Out of memory.";
        case 94: return L"Unable to set the DHCP default gateway.";
        case 95: return L"Unable to set the DHCP default gateway metric.";
        case 96: return L"Unable to set the DNS server search order.";
        case 97: return L"Unable to set the DNS domain.";
        case 100: return L"DHCP is not enabled on this adapter.";
        default: break;
    }
    wchar_t buf[64];
    swprintf_s(buf, L"WMI returned %lu.", rc);
    return buf;
}

SAFEARRAY* makeStringArray(const std::vector<std::wstring>& values) {
    SAFEARRAY* sa = SafeArrayCreateVector(VT_BSTR, 0, static_cast<ULONG>(values.size()));
    if (!sa) return nullptr;
    for (LONG i = 0; i < static_cast<LONG>(values.size()); ++i) {
        BSTR item = SysAllocString(values[static_cast<size_t>(i)].c_str());
        if (!item) { SafeArrayDestroy(sa); return nullptr; }
        HRESULT hr = SafeArrayPutElement(sa, &i, item);  // copies the BSTR
        SysFreeString(item);
        if (FAILED(hr)) { SafeArrayDestroy(sa); return nullptr; }
    }
    return sa;
}

bool putStringArray(IWbemClassObject* params, const wchar_t* name,
                    const std::vector<std::wstring>& values) {
    VARIANT v;
    VariantInit(&v);
    if (values.empty()) {
        // A VT_NULL clears the property; an empty array is rejected by some of
        // these methods, so this is the form that means "remove them all".
        v.vt = VT_NULL;
    } else {
        SAFEARRAY* sa = makeStringArray(values);
        if (!sa) return false;
        v.vt = VT_ARRAY | VT_BSTR;
        v.parray = sa;
    }
    Bstr prop(name);
    HRESULT hr = prop.valid() ? params->Put(prop, 0, &v, 0) : E_OUTOFMEMORY;
    VariantClear(&v);
    return SUCCEEDED(hr);
}

// One WMI session, alive for the length of a single apply().
class WmiSession {
public:
    ~WmiSession() {
        release(services_);
        release(locator_);
        if (comInitialised_) CoUninitialize();
    }

    bool open(std::wstring& error) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) { error = L"CoInitializeEx failed (" + hresultText(hr) + L")."; return false; }
        comInitialised_ = true;

        // RPC_E_TOO_LATE just means another thread got here first, which is
        // fine -- the process-wide blanket is already what we would have set.
        hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                                  RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                                  nullptr, EOAC_NONE, nullptr);
        if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
            error = L"CoInitializeSecurity failed (" + hresultText(hr) + L").";
            return false;
        }

        hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IWbemLocator, reinterpret_cast<void**>(&locator_));
        if (FAILED(hr)) { error = L"WMI locator unavailable (" + hresultText(hr) + L")."; return false; }

        Bstr ns(L"ROOT\\CIMV2");
        if (!ns.valid()) { error = L"Out of memory."; return false; }
        hr = locator_->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services_);
        if (FAILED(hr)) { error = L"Cannot connect to ROOT\\CIMV2 (" + hresultText(hr) + L")."; return false; }

        hr = CoSetProxyBlanket(services_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                               RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                               nullptr, EOAC_NONE);
        if (FAILED(hr)) { error = L"CoSetProxyBlanket failed (" + hresultText(hr) + L")."; return false; }
        return true;
    }

    // The key of Win32_NetworkAdapterConfiguration is Index, which is NOT the
    // IfIndex the IP Helper API reports -- so the adapter is found by its
    // SettingID (the GUID) and identified from there by its own __RELPATH.
    bool findAdapterPath(const std::wstring& settingId, std::wstring& path, std::wstring& error) {
        std::wstring wql = L"SELECT __RELPATH FROM Win32_NetworkAdapterConfiguration WHERE SettingID='"
                           + settingId + L"'";
        Bstr lang(L"WQL");
        Bstr query(wql.c_str());
        if (!lang.valid() || !query.valid()) { error = L"Out of memory."; return false; }

        IEnumWbemClassObject* e = nullptr;
        HRESULT hr = services_->ExecQuery(lang, query,
                                          WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                          nullptr, &e);
        if (FAILED(hr)) { error = L"WMI query failed (" + hresultText(hr) + L")."; return false; }

        IWbemClassObject* obj = nullptr;
        ULONG returned = 0;
        hr = e->Next(WBEM_INFINITE, 1, &obj, &returned);
        release(e);
        if (FAILED(hr) || returned == 0 || !obj) {
            release(obj);
            error = L"WMI has no configuration for adapter " + settingId + L".";
            return false;
        }

        VARIANT v;
        VariantInit(&v);
        hr = obj->Get(L"__RELPATH", 0, &v, nullptr, nullptr);
        bool ok = SUCCEEDED(hr) && v.vt == VT_BSTR && v.bstrVal;
        if (ok) path = v.bstrVal;
        VariantClear(&v);
        release(obj);
        if (!ok) error = L"Could not read the WMI object path for the adapter.";
        return ok;
    }

    // Calls `method` on `path`. `inParams` may be null for methods with no
    // arguments. On a WMI-level failure `error` is set; on a method-level
    // failure the return code lands in `returnCode`.
    bool call(const std::wstring& path, const wchar_t* method, IWbemClassObject* inParams,
              unsigned long& returnCode, std::wstring& error) {
        Bstr objPath(path.c_str());
        Bstr methodName(method);
        if (!objPath.valid() || !methodName.valid()) { error = L"Out of memory."; return false; }

        IWbemClassObject* out = nullptr;
        HRESULT hr = services_->ExecMethod(objPath, methodName, 0, nullptr, inParams, &out, nullptr);
        if (FAILED(hr)) {
            error = std::wstring(method) + L" failed (" + hresultText(hr) + L").";
            release(out);
            return false;
        }

        returnCode = 65;  // "unknown failure" unless the object says otherwise
        if (out) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(out->Get(L"ReturnValue", 0, &v, nullptr, nullptr))) {
                if (v.vt == VT_I4)      returnCode = static_cast<unsigned long>(v.lVal);
                else if (v.vt == VT_UI4) returnCode = v.ulVal;
            }
            VariantClear(&v);
        }
        release(out);
        return true;
    }

    // Builds the in-parameter object for one method of the configuration class.
    bool makeInParams(const wchar_t* method, IWbemClassObject** params, std::wstring& error) {
        *params = nullptr;
        Bstr className(L"Win32_NetworkAdapterConfiguration");
        if (!className.valid()) { error = L"Out of memory."; return false; }

        IWbemClassObject* cls = nullptr;
        HRESULT hr = services_->GetObject(className, 0, nullptr, &cls, nullptr);
        if (FAILED(hr) || !cls) {
            release(cls);
            error = L"Cannot read Win32_NetworkAdapterConfiguration (" + hresultText(hr) + L").";
            return false;
        }

        IWbemClassObject* def = nullptr;
        hr = cls->GetMethod(method, 0, &def, nullptr);
        release(cls);
        if (FAILED(hr) || !def) {
            release(def);
            error = std::wstring(L"Cannot read the signature of ") + method + L".";
            return false;
        }

        hr = def->SpawnInstance(0, params);
        release(def);
        if (FAILED(hr) || !*params) {
            error = std::wstring(L"Cannot build parameters for ") + method + L".";
            return false;
        }
        return true;
    }

private:
    bool comInitialised_ = false;
    IWbemLocator* locator_ = nullptr;
    IWbemServices* services_ = nullptr;
};

}  // namespace

// -------------------------------------------------------------------------

std::vector<AdapterInfo> enumerateAdapters(std::wstring& error) {
    error.clear();
    std::vector<AdapterInfo> adapters;

    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST;
    ULONG size = 16384;
    std::vector<BYTE> buffer(size);
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) {
        wchar_t buf[80];
        swprintf_s(buf, L"GetAdaptersAddresses failed (%lu).", rc);
        error = buf;
        return adapters;
    }

    for (const IP_ADAPTER_ADDRESSES* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         aa != nullptr; aa = aa->Next) {
        if (aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        AdapterInfo info;
        info.friendlyName = aa->FriendlyName ? aa->FriendlyName : L"";
        info.description  = aa->Description ? aa->Description : L"";
        if (aa->AdapterName) {
            // AdapterName is the ANSI form of the GUID, braces included.
            int need = MultiByteToWideChar(CP_ACP, 0, aa->AdapterName, -1, nullptr, 0);
            if (need > 1) {
                std::wstring guid(static_cast<size_t>(need - 1), L'\0');
                MultiByteToWideChar(CP_ACP, 0, aa->AdapterName, -1, guid.data(), need);
                info.settingId = guid;
            }
        }
        info.macAddress  = formatMac(aa->PhysicalAddress, aa->PhysicalAddressLength);
        info.ifIndex     = aa->IfIndex;
        info.dhcpEnabled = (aa->Flags & IP_ADAPTER_DHCP_ENABLED) != 0;
        info.operational = aa->OperStatus == IfOperStatusUp;

        for (const IP_ADAPTER_UNICAST_ADDRESS* ua = aa->FirstUnicastAddress; ua; ua = ua->Next) {
            std::wstring ip = toString(ua->Address.lpSockaddr);
            if (ip.empty()) continue;
            // A 169.254.x.x autoconfiguration address is not something the user
            // set, and offering it back as a static address would be a trap.
            if (ip.rfind(L"169.254.", 0) == 0) continue;
            AddressV4 addr;
            addr.ip = ip;
            addr.mask = maskFromPrefix(ua->OnLinkPrefixLength);
            info.addresses.push_back(addr);
        }
        for (const IP_ADAPTER_GATEWAY_ADDRESS* g = aa->FirstGatewayAddress; g; g = g->Next) {
            std::wstring gw = toString(g->Address.lpSockaddr);
            if (!gw.empty()) info.gateways.push_back(gw);
        }
        for (const IP_ADAPTER_DNS_SERVER_ADDRESS* d = aa->FirstDnsServerAddress; d; d = d->Next) {
            std::wstring dns = toString(d->Address.lpSockaddr);
            if (!dns.empty()) info.dnsServers.push_back(dns);
        }

        if (info.settingId.empty()) continue;  // nothing WMI could be pointed at
        adapters.push_back(std::move(info));
    }

    if (adapters.empty() && error.empty()) error = L"No IPv4-capable adapters found.";
    return adapters;
}

ApplyResult apply(const ApplyRequest& req) {
    ApplyResult result;

    WmiSession wmi;
    if (!wmi.open(result.message)) return result;

    std::wstring path;
    if (!wmi.findAdapterPath(req.settingId, path, result.message)) return result;

    unsigned long rc = 0;

    if (req.useDhcp) {
        if (!wmi.call(path, L"EnableDHCP", nullptr, rc, result.message)) return result;
        if (rc != 0 && rc != 1) { result.message = L"EnableDHCP: " + wmiReturnText(rc); return result; }
        result.rebootRequired = (rc == 1);

        // Hand DNS back to DHCP as well; otherwise a static list set earlier
        // survives the switch and quietly overrides the server's.
        IWbemClassObject* params = nullptr;
        if (!wmi.makeInParams(L"SetDNSServerSearchOrder", &params, result.message)) return result;
        bool put = putStringArray(params, L"DNSServerSearchOrder", req.dnsServers);
        if (put) {
            unsigned long dnsRc = 0;
            std::wstring dnsErr;
            if (wmi.call(path, L"SetDNSServerSearchOrder", params, dnsRc, dnsErr) && dnsRc == 1)
                result.rebootRequired = true;
        }
        release(params);

        result.ok = true;
        result.message = req.dnsServers.empty()
            ? L"DHCP enabled; address and DNS now come from the server."
            : L"DHCP enabled for the address; the DNS servers you entered were kept static.";
        if (result.rebootRequired) result.message += L" A restart is required.";
        return result;
    }

    if (req.addresses.empty()) {
        result.message = L"No static address to apply.";
        return result;
    }

    // EnableStatic takes the whole set at once: element 0 is the primary and
    // the rest are the additional addresses, which is why they are one call.
    std::vector<std::wstring> ips, masks;
    ips.reserve(req.addresses.size());
    masks.reserve(req.addresses.size());
    for (const AddressV4& a : req.addresses) {
        ips.push_back(a.ip);
        masks.push_back(a.mask);
    }

    IWbemClassObject* params = nullptr;
    if (!wmi.makeInParams(L"EnableStatic", &params, result.message)) return result;
    bool built = putStringArray(params, L"IPAddress", ips) &&
                 putStringArray(params, L"SubnetMask", masks);
    if (!built) {
        release(params);
        result.message = L"Could not build the EnableStatic parameters.";
        return result;
    }
    bool called = wmi.call(path, L"EnableStatic", params, rc, result.message);
    release(params);
    if (!called) return result;
    if (rc != 0 && rc != 1) { result.message = L"EnableStatic: " + wmiReturnText(rc); return result; }
    if (rc == 1) result.rebootRequired = true;

    // Gateway. An empty list is the documented way to remove the gateways.
    {
        std::vector<std::wstring> gateways;
        if (!req.gateway.empty()) gateways.push_back(req.gateway);
        IWbemClassObject* gwParams = nullptr;
        if (!wmi.makeInParams(L"SetGateways", &gwParams, result.message)) return result;
        bool put = putStringArray(gwParams, L"DefaultIPGateway", gateways);
        if (put) {
            unsigned long gwRc = 0;
            bool ok = wmi.call(path, L"SetGateways", gwParams, gwRc, result.message);
            release(gwParams);
            if (!ok) return result;
            if (gwRc != 0 && gwRc != 1) {
                result.message = L"The addresses were set, but SetGateways: " + wmiReturnText(gwRc);
                return result;
            }
            if (gwRc == 1) result.rebootRequired = true;
        } else {
            release(gwParams);
            result.message = L"Could not build the SetGateways parameters.";
            return result;
        }
    }

    // DNS.
    {
        IWbemClassObject* dnsParams = nullptr;
        if (!wmi.makeInParams(L"SetDNSServerSearchOrder", &dnsParams, result.message)) return result;
        bool put = putStringArray(dnsParams, L"DNSServerSearchOrder", req.dnsServers);
        if (put) {
            unsigned long dnsRc = 0;
            bool ok = wmi.call(path, L"SetDNSServerSearchOrder", dnsParams, dnsRc, result.message);
            release(dnsParams);
            if (!ok) return result;
            if (dnsRc != 0 && dnsRc != 1) {
                result.message = L"The addresses were set, but SetDNSServerSearchOrder: "
                                 + wmiReturnText(dnsRc);
                return result;
            }
            if (dnsRc == 1) result.rebootRequired = true;
        } else {
            release(dnsParams);
            result.message = L"Could not build the SetDNSServerSearchOrder parameters.";
            return result;
        }
    }

    result.ok = true;
    wchar_t buf[128];
    swprintf_s(buf, L"Applied: %zu address%s, gateway %s.",
               req.addresses.size(),
               req.addresses.size() == 1 ? L"" : L"es",
               req.gateway.empty() ? L"cleared" : req.gateway.c_str());
    result.message = buf;
    if (result.rebootRequired) result.message += L" A restart is required.";
    return result;
}

}  // namespace net
