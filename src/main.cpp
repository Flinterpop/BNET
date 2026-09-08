// BNET -- one dialog that does what the Windows IPv4 property sheets do.
//
// The point of the app is the absence of navigation: adapter, DHCP-or-static,
// address, mask, gateway, DNS and the whole list of additional addresses are
// all on one surface, and Apply commits them together.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>

#include <string>
#include <thread>
#include <vector>

#include "NetConfig.h"
#include "resource.h"

namespace {

constexpr UINT WM_APP_APPLY_DONE = WM_APP + 1;

std::vector<net::AdapterInfo> g_adapters;
bool g_applying = false;
net::ApplyRequest g_lastRequest;   // what the last Apply asked for, for verification

// --------------------------------------------------------------- utilities

std::wstring ipToString(DWORD hostOrder) {
    wchar_t buf[INET_ADDRSTRLEN];
    swprintf_s(buf, L"%lu.%lu.%lu.%lu",
               (hostOrder >> 24) & 0xFF, (hostOrder >> 16) & 0xFF,
               (hostOrder >> 8) & 0xFF, hostOrder & 0xFF);
    return buf;
}

bool ipToHost(const std::wstring& text, DWORD& hostOrder) {
    IN_ADDR a{};
    if (text.empty() || InetPtonW(AF_INET, text.c_str(), &a) != 1) return false;
    hostOrder = ntohl(a.S_un.S_addr);
    return true;
}

void setIpField(HWND dlg, int id, const std::wstring& text) {
    HWND h = GetDlgItem(dlg, id);
    DWORD host = 0;
    if (ipToHost(text, host))
        SendMessageW(h, IPM_SETADDRESS, 0, static_cast<LPARAM>(host));
    else
        SendMessageW(h, IPM_CLEARADDRESS, 0, 0);
}

// Three outcomes, not two: filled, deliberately blank, or half-typed.
enum class FieldState { Filled, Blank, Partial };

FieldState getIpField(HWND dlg, int id, std::wstring& text) {
    DWORD addr = 0;
    LRESULT filled = SendMessageW(GetDlgItem(dlg, id), IPM_GETADDRESS, 0,
                                  reinterpret_cast<LPARAM>(&addr));
    text.clear();
    if (filled == 0) return FieldState::Blank;
    if (filled < 4) return FieldState::Partial;
    text = ipToString(addr);
    return FieldState::Filled;
}

// A mask is legal only if it is a run of ones followed by a run of zeroes.
bool isContiguousMask(DWORD mask) {
    if (mask == 0 || mask == 0xFFFFFFFFu) return false;
    DWORD inverted = ~mask;
    return (inverted & (inverted + 1)) == 0;
}

void setStatus(HWND dlg, const std::wstring& text) {
    SetDlgItemTextW(dlg, IDC_STATUS, text.c_str());
}

void warn(HWND dlg, const std::wstring& text) {
    MessageBoxW(dlg, text.c_str(), L"BNET", MB_OK | MB_ICONWARNING);
}

// ------------------------------------------------------------- list helpers

int listCount(HWND dlg) {
    return static_cast<int>(SendDlgItemMessageW(dlg, IDC_LIST, LVM_GETITEMCOUNT, 0, 0));
}

std::wstring listText(HWND dlg, int row, int column) {
    wchar_t buf[64] = {};
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = buf;
    item.cchTextMax = static_cast<int>(ARRAYSIZE(buf));
    SendDlgItemMessageW(dlg, IDC_LIST, LVM_GETITEMTEXTW, static_cast<WPARAM>(row),
                        reinterpret_cast<LPARAM>(&item));
    return buf;
}

void listAdd(HWND dlg, const std::wstring& ip, const std::wstring& mask,
             const std::wstring& state = std::wstring()) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = listCount(dlg);
    item.pszText = const_cast<LPWSTR>(ip.c_str());
    int row = static_cast<int>(SendDlgItemMessageW(dlg, IDC_LIST, LVM_INSERTITEMW, 0,
                                                   reinterpret_cast<LPARAM>(&item)));
    if (row < 0) return;
    item.iItem = row;
    item.iSubItem = 1;
    item.pszText = const_cast<LPWSTR>(mask.c_str());
    SendDlgItemMessageW(dlg, IDC_LIST, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
                        reinterpret_cast<LPARAM>(&item));
    item.iSubItem = 2;
    item.pszText = const_cast<LPWSTR>(state.c_str());
    SendDlgItemMessageW(dlg, IDC_LIST, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
                        reinterpret_cast<LPARAM>(&item));
}

int listSelection(HWND dlg) {
    return static_cast<int>(SendDlgItemMessageW(dlg, IDC_LIST, LVM_GETNEXTITEM,
                                                static_cast<WPARAM>(-1), LVNI_SELECTED));
}

std::vector<net::AddressV4> listContents(HWND dlg) {
    std::vector<net::AddressV4> out;
    int rows = listCount(dlg);
    out.reserve(static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i) out.push_back({listText(dlg, i, 0), listText(dlg, i, 1)});
    return out;
}

// ----------------------------------------------------------- dialog filling

void enableStaticFields(HWND dlg, bool on) {
    static const int ids[] = {IDC_IP, IDC_MASK, IDC_GW, IDC_DNS1, IDC_DNS2,
                              IDC_LBL_IP, IDC_LBL_MASK, IDC_LBL_GW, IDC_LBL_DNS1, IDC_LBL_DNS2,
                              IDC_LIST, IDC_ADD_IP, IDC_ADD_MASK, IDC_ADD, IDC_REMOVE};
    for (int id : ids) EnableWindow(GetDlgItem(dlg, id), on ? TRUE : FALSE);
}

void showAdapter(HWND dlg, const net::AdapterInfo& a) {
    std::wstring info = a.description;
    if (!a.macAddress.empty()) info += L"   MAC " + a.macAddress;
    info += a.operational ? L"   [connected]" : L"   [not connected]";
    info += a.dhcpEnabled ? L"   currently DHCP" : L"   currently static";
    // A primary address the stack has but has not accepted is the whole reason
    // this dialog and ipconfig can disagree, so it is said out loud.
    if (!a.addresses.empty() && a.addresses[0].state != net::AddressState::Preferred)
        info += std::wstring(L"   primary is ") + net::stateText(a.addresses[0].state);
    SetDlgItemTextW(dlg, IDC_ADAPTER_INFO, info.c_str());

    CheckRadioButton(dlg, IDC_RAD_DHCP, IDC_RAD_STATIC,
                     a.dhcpEnabled ? IDC_RAD_DHCP : IDC_RAD_STATIC);
    enableStaticFields(dlg, !a.dhcpEnabled);

    // The current values are shown either way: under DHCP they are what the
    // server handed out, which is the sane starting point for going static.
    setIpField(dlg, IDC_IP, a.addresses.empty() ? L"" : a.addresses[0].ip);
    setIpField(dlg, IDC_MASK, a.addresses.empty() ? L"" : a.addresses[0].mask);
    setIpField(dlg, IDC_GW, a.gateways.empty() ? L"" : a.gateways[0]);
    setIpField(dlg, IDC_DNS1, a.dnsServers.size() > 0 ? a.dnsServers[0] : L"");
    setIpField(dlg, IDC_DNS2, a.dnsServers.size() > 1 ? a.dnsServers[1] : L"");
    setIpField(dlg, IDC_ADD_IP, L"");
    setIpField(dlg, IDC_ADD_MASK, L"");

    SendDlgItemMessageW(dlg, IDC_LIST, LVM_DELETEALLITEMS, 0, 0);
    for (size_t i = 1; i < a.addresses.size(); ++i) {
        const net::AddressV4& extra = a.addresses[i];
        listAdd(dlg, extra.ip, extra.mask,
                extra.state == net::AddressState::Preferred ? L"" : net::stateText(extra.state));
    }
}

void loadAdapters(HWND dlg, const std::wstring& preferSettingId) {
    std::wstring error;
    g_adapters = net::enumerateAdapters(error);

    HWND combo = GetDlgItem(dlg, IDC_ADAPTER);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const net::AdapterInfo& a : g_adapters) {
        std::wstring label = a.friendlyName.empty() ? a.description : a.friendlyName;
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }

    if (g_adapters.empty()) {
        SetDlgItemTextW(dlg, IDC_ADAPTER_INFO, L"");
        enableStaticFields(dlg, false);
        EnableWindow(GetDlgItem(dlg, IDC_APPLY), FALSE);
        setStatus(dlg, error.empty() ? L"No adapters." : error);
        return;
    }

    int select = 0;
    for (size_t i = 0; i < g_adapters.size(); ++i) {
        if (!preferSettingId.empty() && g_adapters[i].settingId == preferSettingId) {
            select = static_cast<int>(i);
            break;
        }
        // Otherwise land on the first adapter that is actually up and addressed.
        if (preferSettingId.empty() && select == 0 &&
            g_adapters[i].operational && !g_adapters[i].addresses.empty()) {
            select = static_cast<int>(i);
        }
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(select), 0);
    EnableWindow(GetDlgItem(dlg, IDC_APPLY), TRUE);
    showAdapter(dlg, g_adapters[static_cast<size_t>(select)]);
}

const net::AdapterInfo* currentAdapter(HWND dlg) {
    LRESULT sel = SendDlgItemMessageW(dlg, IDC_ADAPTER, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR || sel < 0 || static_cast<size_t>(sel) >= g_adapters.size()) return nullptr;
    return &g_adapters[static_cast<size_t>(sel)];
}

// ------------------------------------------------------------------- apply

void setBusy(HWND dlg, bool busy) {
    g_applying = busy;
    static const int ids[] = {IDC_ADAPTER, IDC_REFRESH, IDC_RAD_DHCP, IDC_RAD_STATIC, IDC_APPLY};
    for (int id : ids) EnableWindow(GetDlgItem(dlg, id), busy ? FALSE : TRUE);
    if (busy)
        enableStaticFields(dlg, false);
    else if (currentAdapter(dlg))
        enableStaticFields(dlg, IsDlgButtonChecked(dlg, IDC_RAD_STATIC) == BST_CHECKED);
    SetCursor(LoadCursor(nullptr, busy ? IDC_WAIT : IDC_ARROW));
}

// Reads a required address field, complaining precisely about what is wrong.
bool readRequired(HWND dlg, int id, const wchar_t* label, std::wstring& out) {
    switch (getIpField(dlg, id, out)) {
        case FieldState::Filled:
            return true;
        case FieldState::Blank:
            warn(dlg, std::wstring(L"Enter a ") + label + L".");
            break;
        case FieldState::Partial:
            warn(dlg, std::wstring(L"The ") + label + L" is incomplete.");
            break;
    }
    SetFocus(GetDlgItem(dlg, id));
    return false;
}

bool readOptional(HWND dlg, int id, const wchar_t* label, std::wstring& out) {
    switch (getIpField(dlg, id, out)) {
        case FieldState::Filled:
        case FieldState::Blank:
            return true;
        case FieldState::Partial:
            warn(dlg, std::wstring(L"The ") + label + L" is incomplete. "
                      L"Clear it, or finish typing it.");
            SetFocus(GetDlgItem(dlg, id));
            break;
    }
    return false;
}

bool checkMask(HWND dlg, int id, const std::wstring& mask) {
    DWORD value = 0;
    if (ipToHost(mask, value) && isContiguousMask(value)) return true;
    warn(dlg, L"\"" + mask + L"\" is not a valid subnet mask. A mask must be a run of "
              L"ones followed by a run of zeroes, such as 255.255.255.0.");
    SetFocus(GetDlgItem(dlg, id));
    return false;
}

void onAdd(HWND dlg) {
    std::wstring ip, mask;
    if (!readRequired(dlg, IDC_ADD_IP, L"IP address to add", ip)) return;
    if (!readRequired(dlg, IDC_ADD_MASK, L"subnet mask for the address to add", mask)) return;
    if (!checkMask(dlg, IDC_ADD_MASK, mask)) return;

    std::wstring primary;
    if (getIpField(dlg, IDC_IP, primary) == FieldState::Filled && primary == ip) {
        warn(dlg, L"That is already the primary address.");
        return;
    }
    for (const net::AddressV4& existing : listContents(dlg)) {
        if (existing.ip == ip) {
            warn(dlg, L"That address is already in the list.");
            return;
        }
    }

    listAdd(dlg, ip, mask);
    setIpField(dlg, IDC_ADD_IP, L"");
    setIpField(dlg, IDC_ADD_MASK, L"");
    SetFocus(GetDlgItem(dlg, IDC_ADD_IP));
    setStatus(dlg, L"Added " + ip + L". Nothing is committed until you press Apply.");
}

void onRemove(HWND dlg) {
    int row = listSelection(dlg);
    if (row < 0) {
        warn(dlg, L"Select an address in the list first.");
        return;
    }
    std::wstring ip = listText(dlg, row, 0);
    SendDlgItemMessageW(dlg, IDC_LIST, LVM_DELETEITEM, static_cast<WPARAM>(row), 0);
    setStatus(dlg, L"Removed " + ip + L". Nothing is committed until you press Apply.");
}

// Double-clicking a row lifts it back into the entry fields, which is the
// closest thing to editing in place without opening a second dialog.
void onEditInPlace(HWND dlg) {
    int row = listSelection(dlg);
    if (row < 0) return;
    std::wstring ip = listText(dlg, row, 0);
    std::wstring mask = listText(dlg, row, 1);
    SendDlgItemMessageW(dlg, IDC_LIST, LVM_DELETEITEM, static_cast<WPARAM>(row), 0);
    setIpField(dlg, IDC_ADD_IP, ip);
    setIpField(dlg, IDC_ADD_MASK, mask);
    SetFocus(GetDlgItem(dlg, IDC_ADD_IP));
    setStatus(dlg, L"Editing " + ip + L". Press Add to put it back.");
}

bool buildRequest(HWND dlg, net::ApplyRequest& req) {
    const net::AdapterInfo* adapter = currentAdapter(dlg);
    if (!adapter) return false;
    req.settingId = adapter->settingId;
    req.useDhcp = IsDlgButtonChecked(dlg, IDC_RAD_DHCP) == BST_CHECKED;

    std::wstring dns1, dns2;
    if (!readOptional(dlg, IDC_DNS1, L"preferred DNS server", dns1)) return false;
    if (!readOptional(dlg, IDC_DNS2, L"alternate DNS server", dns2)) return false;
    if (!dns1.empty()) req.dnsServers.push_back(dns1);
    if (!dns2.empty()) {
        if (dns1.empty()) {
            warn(dlg, L"Fill in the preferred DNS server before the alternate one.");
            return false;
        }
        req.dnsServers.push_back(dns2);
    }

    if (req.useDhcp) return true;

    std::wstring ip, mask, gateway;
    if (!readRequired(dlg, IDC_IP, L"IP address", ip)) return false;
    if (!readRequired(dlg, IDC_MASK, L"subnet mask", mask)) return false;
    if (!checkMask(dlg, IDC_MASK, mask)) return false;
    if (!readOptional(dlg, IDC_GW, L"default gateway", gateway)) return false;

    req.addresses.push_back({ip, mask});
    for (const net::AddressV4& extra : listContents(dlg)) {
        if (!checkMask(dlg, IDC_LIST, extra.mask)) return false;
        if (extra.ip == ip) {
            warn(dlg, L"The primary address " + ip + L" is also in the additional list.");
            return false;
        }
        req.addresses.push_back(extra);
    }
    req.gateway = gateway;

    // Same warning the Windows dialog gives, and for the same reason: a gateway
    // outside every configured subnet is unreachable and silently does nothing.
    if (!gateway.empty()) {
        DWORD gwValue = 0;
        bool reachable = false;
        if (ipToHost(gateway, gwValue)) {
            for (const net::AddressV4& a : req.addresses) {
                DWORD addrValue = 0, maskValue = 0;
                if (ipToHost(a.ip, addrValue) && ipToHost(a.mask, maskValue) &&
                    (addrValue & maskValue) == (gwValue & maskValue)) {
                    reachable = true;
                    break;
                }
            }
        }
        if (!reachable) {
            std::wstring text = L"The gateway " + gateway +
                                L" is not on the same subnet as any address you entered, so it "
                                L"will not be reachable.\n\nApply it anyway?";
            if (MessageBoxW(dlg, text.c_str(), L"BNET", MB_YESNO | MB_ICONWARNING) != IDYES)
                return false;
        }
    }
    return true;
}

void onApply(HWND dlg) {
    net::ApplyRequest req;
    if (!buildRequest(dlg, req)) return;
    g_lastRequest = req;

    setBusy(dlg, true);
    setStatus(dlg, req.useDhcp ? L"Switching to DHCP..." : L"Applying addresses...");

    // WMI takes seconds on a busy adapter, so it runs off the UI thread and
    // posts the outcome back. The result is heap-owned across the handoff.
    net::ApplyRequest* job = new net::ApplyRequest(req);
    std::thread([job, dlg] {
        net::ApplyResult* result = new net::ApplyResult(net::apply(*job));
        delete job;
        if (!PostMessageW(dlg, WM_APP_APPLY_DONE, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

// WMI reporting success only means the request was accepted, not that the stack
// took it -- an address can be written and then rejected by duplicate address
// detection, or written to the registry only. So the applied set is compared
// against what the adapter actually reports, and any gap is said plainly. This
// is the difference between the dialog agreeing with ipconfig and merely
// claiming to.
void verifyApplied(HWND dlg, const net::ApplyRequest& req) {
    if (req.useDhcp) return;
    const net::AdapterInfo* adapter = currentAdapter(dlg);
    if (!adapter) return;

    std::wstring missing, rejected;
    for (const net::AddressV4& wanted : req.addresses) {
        const net::AddressV4* found = nullptr;
        for (const net::AddressV4& have : adapter->addresses) {
            if (have.ip == wanted.ip) { found = &have; break; }
        }
        if (!found) {
            missing += L"    " + wanted.ip + L"\n";
        } else if (found->state != net::AddressState::Preferred) {
            rejected += L"    " + wanted.ip + L"  (" + net::stateText(found->state) + L")\n";
        }
    }

    if (!missing.empty()) {
        setStatus(dlg, L"Windows did not keep every address -- see the message.");
        MessageBoxW(dlg,
                    (L"Windows accepted the request but these addresses are not on the "
                     L"adapter:\n\n" + missing +
                     L"\nThere is no separate activation step, so this means the stack "
                     L"never took them. The usual causes are:\n\n"
                     L"    - a restart is pending, and the change is in the registry only\n"
                     L"    - something reverted it: group policy, a VPN client, NIC teaming\n"
                     L"      or a Hyper-V switch owning the adapter\n\n"
                     L"Confirm with:  netsh interface ipv4 show addresses").c_str(),
                    L"BNET -- not all addresses took", MB_OK | MB_ICONERROR);
        return;
    }

    if (!rejected.empty()) {
        setStatus(dlg, L"Applied, but the network rejected an address.");
        MessageBoxW(dlg,
                    (L"These addresses are configured but the network has not accepted "
                     L"them:\n\n" + rejected +
                     L"\nA duplicate means another host already has that address, so "
                     L"ipconfig will not show it as usable and it will carry no traffic. "
                     L"Pick a free address, or find the host holding it.").c_str(),
                    L"BNET -- address not usable", MB_OK | MB_ICONWARNING);
    }
}

void onApplyDone(HWND dlg, LPARAM lparam) {
    net::ApplyResult* result = reinterpret_cast<net::ApplyResult*>(lparam);
    std::wstring settingId;
    if (const net::AdapterInfo* a = currentAdapter(dlg)) settingId = a->settingId;

    setBusy(dlg, false);
    bool ok = result->ok;
    bool rebootRequired = result->rebootRequired;
    std::wstring message = result->message;
    delete result;

    if (!ok) {
        setStatus(dlg, L"Not applied: " + message);
        MessageBoxW(dlg, message.c_str(), L"BNET -- not applied", MB_OK | MB_ICONERROR);
        loadAdapters(dlg, settingId);
        return;
    }

    // Re-read rather than trust the request: this is the only honest confirmation.
    loadAdapters(dlg, settingId);
    setStatus(dlg, message);

    // A restart-required result means the registry has the change and the running
    // stack does not, which looks exactly like the app lying. Too important to
    // leave as the tail of a status line that may be clipped.
    if (rebootRequired) {
        MessageBoxW(dlg,
                    L"Windows stored the change but asked for a restart before it takes "
                    L"effect.\n\nUntil then the registry and the running stack disagree, "
                    L"so ipconfig will not show the new configuration.",
                    L"BNET -- restart required", MB_OK | MB_ICONINFORMATION);
        return;
    }

    verifyApplied(dlg, g_lastRequest);
}

// ------------------------------------------------------------- dialog proc

INT_PTR CALLBACK DlgProc(HWND dlg, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_INITDIALOG: {
            SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(LoadIcon(nullptr, IDI_APPLICATION)));
            HWND list = GetDlgItem(dlg, IDC_LIST);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.cx = 220;
            column.pszText = const_cast<LPWSTR>(L"IP address");
            column.iSubItem = 0;
            ListView_InsertColumn(list, 0, &column);
            column.cx = 220;
            column.pszText = const_cast<LPWSTR>(L"Subnet mask");
            column.iSubItem = 1;
            ListView_InsertColumn(list, 1, &column);
            column.cx = 150;
            column.pszText = const_cast<LPWSTR>(L"State");
            column.iSubItem = 2;
            ListView_InsertColumn(list, 2, &column);

            loadAdapters(dlg, L"");
            setStatus(dlg, L"Ready. Changes are committed only when you press Apply.");
            return TRUE;
        }

        case WM_APP_APPLY_DONE:
            onApplyDone(dlg, lparam);
            return TRUE;

        case WM_SETCURSOR:
            if (g_applying) {
                SetCursor(LoadCursor(nullptr, IDC_WAIT));
                SetWindowLongPtr(dlg, DWLP_MSGRESULT, static_cast<LONG_PTR>(TRUE));
                return TRUE;
            }
            return FALSE;

        case WM_NOTIFY: {
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lparam);
            if (header->idFrom == static_cast<UINT_PTR>(IDC_LIST) && header->code == NM_DBLCLK) {
                onEditInPlace(dlg);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDC_ADAPTER:
                    if (HIWORD(wparam) == CBN_SELCHANGE) {
                        if (const net::AdapterInfo* a = currentAdapter(dlg)) showAdapter(dlg, *a);
                    }
                    return TRUE;

                case IDC_REFRESH: {
                    std::wstring settingId;
                    if (const net::AdapterInfo* a = currentAdapter(dlg)) settingId = a->settingId;
                    loadAdapters(dlg, settingId);
                    setStatus(dlg, L"Re-read from the system.");
                    return TRUE;
                }

                case IDC_RAD_DHCP:
                    enableStaticFields(dlg, false);
                    return TRUE;

                case IDC_RAD_STATIC:
                    enableStaticFields(dlg, true);
                    SetFocus(GetDlgItem(dlg, IDC_IP));
                    return TRUE;

                case IDC_ADD:
                    onAdd(dlg);
                    return TRUE;

                case IDC_REMOVE:
                    onRemove(dlg);
                    return TRUE;

                case IDC_APPLY:
                    onApply(dlg);
                    return TRUE;

                case IDCANCEL:
                    if (g_applying) {
                        MessageBeep(MB_ICONWARNING);
                        return TRUE;
                    }
                    EndDialog(dlg, 0);
                    return TRUE;

                default:
                    return FALSE;
            }

        case WM_CLOSE:
            if (g_applying) {
                MessageBeep(MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(dlg, 0);
            return TRUE;

        default:
            return FALSE;
    }
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_INTERNET_CLASSES;
    InitCommonControlsEx(&controls);

    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, DlgProc, 0);
    return 0;
}
