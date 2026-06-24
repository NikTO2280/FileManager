#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0501   // Поддержка Windows XP
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>

#pragma comment(linker, "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

HWND g_hWnd = nullptr;
HWND g_hListView = nullptr;
HWND g_hAddressBar = nullptr;
HWND g_hStatus = nullptr;
HWND g_hToolbar = nullptr;
WNDPROC g_oldEditProc = nullptr;
WNDPROC g_oldListViewProc = nullptr;

std::vector<std::wstring> g_history;
int g_historyPos = -1;
std::wstring g_currentPath;
bool g_showHidden = false;

// ----------------------------------------------------------------------
// Прототипы
// ----------------------------------------------------------------------
void ShowDrives();
void NavigateTo(const std::wstring& path, bool addToHistory = true);
void PopulateListView(const std::wstring& path);
void GoUp();
void EjectDrive(wchar_t driveLetter = L'\0');
void CopySelectedFilesToClipboard();
void PasteFilesFromClipboard();
void DeleteSelectedFiles(bool permanent);

// ----------------------------------------------------------------------
// Вспомогательные
// ----------------------------------------------------------------------
std::wstring FormatSize(ULONGLONG size) {
    wchar_t buf[128];
    if (size < 1024)
        swprintf_s(buf, L"%llu B", size);
    else if (size < 1024 * 1024)
        swprintf_s(buf, L"%llu KB", size / 1024);
    else if (size < 1024 * 1024 * 1024)
        swprintf_s(buf, L"%llu MB", size / (1024 * 1024));
    else
        swprintf_s(buf, L"%llu GB", size / (1024 * 1024 * 1024));
    return std::wstring(buf);
}

std::wstring FormatFileTime(const FILETIME& ft) {
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    wchar_t buf[100];
    swprintf_s(buf, L"%02d.%02d.%04d %02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    return std::wstring(buf);
}

std::wstring GetFileTypeString(const std::wstring& path) {
    SHFILEINFOW sfi = { 0 };
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_TYPENAME))
            return sfi.szTypeName[0] ? sfi.szTypeName : L"File";
        return L"File";
    }
    if (SHGetFileInfoW(path.c_str(), attrs, &sfi, sizeof(sfi), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES)) {
        if (sfi.szTypeName[0] != 0)
            return sfi.szTypeName;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        return L"Folder";
    return L"File";
}

void UpdateStatus(const std::wstring& path) {
    if (!g_hStatus) return;
    std::wstring statusText;

    if (path.empty()) {
        statusText = L"Ready";
    } else {
        ULARGE_INTEGER freeBytes, totalBytes, totalFree;
        std::wstring root = path.substr(0, 3);
        if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &totalBytes, &totalFree)) {
            wchar_t buf[256];
            swprintf_s(buf, L"Free: %I64u MB / %I64u MB",
                       freeBytes.QuadPart / (1024 * 1024),
                       totalBytes.QuadPart / (1024 * 1024));
            statusText = buf;
        } else {
            statusText = L"Cannot get disk info";
        }
    }
    SendMessageW(g_hStatus, SB_SETTEXT, 0, (LPARAM)statusText.c_str());
}

// ----------------------------------------------------------------------
// Eject CD-ROM / Removable (с возможностью указать букву)
// ----------------------------------------------------------------------
void EjectDrive(wchar_t driveLetter) {
    wchar_t drive = driveLetter;
    if (drive == L'\0') {
        if (!g_currentPath.empty()) {
            drive = g_currentPath[0];
        } else {
            DWORD drives = GetLogicalDrives();
            for (int i = 0; i < 26; i++) {
                if (drives & (1 << i)) {
                    wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
                    UINT type = GetDriveTypeW(root);
                    if (type == DRIVE_CDROM || type == DRIVE_REMOVABLE) {
                        drive = (wchar_t)(L'A' + i);
                        break;
                    }
                }
            }
        }
    }

    if (drive == L'\0') {
        MessageBoxW(g_hWnd, L"No removable or CD/DVD drive found", L"Info", MB_OK);
        return;
    }

    std::wstring root = std::wstring(1, drive) + L":\\";
    UINT type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_CDROM && type != DRIVE_REMOVABLE) {
        MessageBoxW(g_hWnd, L"Drive is not removable or CD/DVD", L"Info", MB_OK);
        return;
    }

    std::wstring devPath = L"\\\\.\\" + std::wstring(1, drive) + L":";
    HANDLE hDevice = CreateFileW(devPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        ShellExecuteW(g_hWnd, L"Eject", root.c_str(), NULL, NULL, SW_SHOW);
        MessageBoxW(g_hWnd, L"Eject command sent via Shell", L"Info", MB_OK);
        return;
    }
    DWORD bytesReturned;
    BOOL result = DeviceIoControl(hDevice, IOCTL_STORAGE_EJECT_MEDIA,
        NULL, 0, NULL, 0, &bytesReturned, NULL);
    CloseHandle(hDevice);

    if (!result) {
        ShellExecuteW(g_hWnd, L"Eject", root.c_str(), NULL, NULL, SW_SHOW);
        MessageBoxW(g_hWnd, L"Eject command sent via Shell", L"Info", MB_OK);
    } else {
        MessageBoxW(g_hWnd, L"Drive ejected successfully", L"Info", MB_OK);
    }

    ShowDrives();
    if (!g_currentPath.empty() && towupper(g_currentPath[0]) == towupper(drive)) {
        NavigateTo(L"", true);
    }
}

// ----------------------------------------------------------------------
// Уникальное имя для копии
// ----------------------------------------------------------------------
std::wstring MakeUniqueFileName(const std::wstring& destFolder, const std::wstring& fileName) {
    size_t dotPos = fileName.find_last_of(L'.');
    std::wstring baseName, ext;
    if (dotPos != std::wstring::npos) {
        baseName = fileName.substr(0, dotPos);
        ext = fileName.substr(dotPos);
    } else {
        baseName = fileName;
        ext = L"";
    }

    std::wstring newName = fileName;
    int counter = 1;
    while (true) {
        std::wstring fullPath = destFolder + L"\\" + newName;
        DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            return newName;
        }
        wchar_t buf[256];
        swprintf_s(buf, L"%s-%d%s", baseName.c_str(), counter, ext.c_str());
        newName = buf;
        counter++;
    }
}

// ----------------------------------------------------------------------
// Навигация и отображение
// ----------------------------------------------------------------------
void NavigateTo(const std::wstring& path, bool addToHistory) {
    if (path.empty()) {
        g_currentPath = L"";
        if (addToHistory) {
            if (g_historyPos < (int)g_history.size() - 1)
                g_history.erase(g_history.begin() + g_historyPos + 1, g_history.end());
            g_history.push_back(L"");
            g_historyPos = (int)g_history.size() - 1;
        }
        SetWindowTextW(g_hAddressBar, L"");
        ShowDrives();
        UpdateStatus(L"");
        return;
    }

    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hWnd, L"Folder does not exist", L"Error", MB_OK);
        return;
    }

    g_currentPath = path;
    if (addToHistory) {
        if (g_historyPos < (int)g_history.size() - 1)
            g_history.erase(g_history.begin() + g_historyPos + 1, g_history.end());
        g_history.push_back(path);
        g_historyPos = (int)g_history.size() - 1;
    }
    SetWindowTextW(g_hAddressBar, path.c_str());
    PopulateListView(path);
}

void GoBack() {
    if (g_historyPos > 0) {
        g_historyPos--;
        NavigateTo(g_history[g_historyPos], false);
    }
}

void GoForward() {
    if (g_historyPos < (int)g_history.size() - 1) {
        g_historyPos++;
        NavigateTo(g_history[g_historyPos], false);
    }
}

void GoUp() {
    if (g_currentPath.empty()) return;
    size_t pos = g_currentPath.find_last_of(L'\\');
    if (pos != std::wstring::npos && pos > 2) {
        std::wstring parent = g_currentPath.substr(0, pos);
        NavigateTo(parent, true);
    } else {
        NavigateTo(L"", true);
    }
}

void PopulateListView(const std::wstring& path) {
    SendMessageW(g_hListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hListView);

    if (path.length() > 3) {
        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem = 0;
        lvi.iSubItem = 0;
        std::wstring up = L"..";
        lvi.pszText = (LPWSTR)up.c_str();
        lvi.lParam = (LPARAM)new std::wstring(L"..");
        SHFILEINFOW sfi = { 0 };
        SHGetFileInfoW(L"folder", 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON);
        int idx = ImageList_AddIcon((HIMAGELIST)SendMessageW(g_hListView, LVM_GETIMAGELIST, LVSIL_SMALL, 0), sfi.hIcon);
        DestroyIcon(sfi.hIcon);
        lvi.iImage = idx;
        ListView_InsertItem(g_hListView, &lvi);
    }

    std::wstring searchPath = path + L"\\*";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        SendMessageW(g_hListView, WM_SETREDRAW, TRUE, 0);
        UpdateStatus(path);
        return;
    }

    struct FileItem {
        std::wstring path;
        FILETIME ft;
        bool isDir;
        DWORD attrs;
        ULARGE_INTEGER size; // размер из WIN32_FIND_DATA
    };
    std::vector<FileItem> items;

    do {
        if (wcscmp(findData.cFileName, L".") == 0) continue;
        std::wstring fullPath = path + L"\\" + findData.cFileName;
        if (!g_showHidden) {
            if (findData.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
                continue;
        }
        FileItem item;
        item.path = fullPath;
        item.ft = findData.ftLastWriteTime;
        item.isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.attrs = findData.dwFileAttributes;
        item.size.LowPart = findData.nFileSizeLow;
        item.size.HighPart = findData.nFileSizeHigh;
        items.push_back(item);
    } while (FindNextFileW(hFind, &findData));
    FindClose(hFind);

    std::sort(items.begin(), items.end(), [](const FileItem& a, const FileItem& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
    });

    HIMAGELIST hImageList = (HIMAGELIST)SendMessageW(g_hListView, LVM_GETIMAGELIST, LVSIL_SMALL, 0);
    int itemIndex = (path.length() > 3) ? 1 : 0;

    for (const auto& item : items) {
        const wchar_t* fileName = wcsrchr(item.path.c_str(), L'\\');
        if (!fileName) fileName = item.path.c_str();
        else fileName++;

        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem = itemIndex++;
        lvi.iSubItem = 0;
        lvi.pszText = (LPWSTR)fileName;
        lvi.lParam = (LPARAM)new std::wstring(item.path);

        SHFILEINFOW sfi = { 0 };
        SHGetFileInfoW(item.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON);
        int idx = ImageList_AddIcon(hImageList, sfi.hIcon);
        DestroyIcon(sfi.hIcon);
        lvi.iImage = idx;
        ListView_InsertItem(g_hListView, &lvi);

        std::wstring sizeStr;
        if (!item.isDir && item.size.QuadPart > 0) {
            sizeStr = FormatSize(item.size.QuadPart);
        }
        ListView_SetItemText(g_hListView, lvi.iItem, 1, (LPWSTR)sizeStr.c_str());

        std::wstring typeStr = GetFileTypeString(item.path);
        ListView_SetItemText(g_hListView, lvi.iItem, 2, (LPWSTR)typeStr.c_str());

        std::wstring dateStr = FormatFileTime(item.ft);
        ListView_SetItemText(g_hListView, lvi.iItem, 3, (LPWSTR)dateStr.c_str());
    }

    SendMessageW(g_hListView, WM_SETREDRAW, TRUE, 0);
    UpdateStatus(path);
}

void ShowDrives() {
    SendMessageW(g_hListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hListView);

    DWORD drives = GetLogicalDrives();
    int itemIndex = 0;
    for (int i = 0; i < 26; i++) {
        if (drives & (1 << i)) {
            wchar_t drive[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
            std::wstring root = drive;
            UINT type = GetDriveTypeW(root.c_str());
            if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) continue;

            wchar_t volumeName[256] = { 0 };
            wchar_t fileSystem[256] = { 0 };
            DWORD serial, maxComLen, fileSysFlags;
            GetVolumeInformationW(root.c_str(), volumeName, 256, &serial, &maxComLen, &fileSysFlags, fileSystem, 256);

            std::wstring displayName;
            if (wcslen(volumeName) > 0) {
                displayName = std::wstring(volumeName) + L" (" + (wchar_t)(L'A' + i) + L":)";
            } else {
                switch (type) {
                case DRIVE_REMOVABLE: displayName = L"Removable (" + std::wstring(1, (wchar_t)(L'A' + i)) + L":)"; break;
                case DRIVE_FIXED: displayName = L"Local Disk (" + std::wstring(1, (wchar_t)(L'A' + i)) + L":)"; break;
                case DRIVE_REMOTE: displayName = L"Network (" + std::wstring(1, (wchar_t)(L'A' + i)) + L":)"; break;
                case DRIVE_CDROM: displayName = L"CD-ROM (" + std::wstring(1, (wchar_t)(L'A' + i)) + L":)"; break;
                case DRIVE_RAMDISK: displayName = L"RAM Disk (" + std::wstring(1, (wchar_t)(L'A' + i)) + L":)"; break;
                default: displayName = L"Drive (" + std::wstring(1, (wchar_t)(L'A' + i)) + L":)"; break;
                }
            }

            LVITEMW lvi = { 0 };
            lvi.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
            lvi.iItem = itemIndex;
            lvi.iSubItem = 0;
            lvi.pszText = (LPWSTR)displayName.c_str();
            lvi.lParam = (LPARAM)new std::wstring(root);

            SHFILEINFOW sfi = { 0 };
            SHGetFileInfoW(root.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON);
            HIMAGELIST hImageList = (HIMAGELIST)SendMessageW(g_hListView, LVM_GETIMAGELIST, LVSIL_SMALL, 0);
            int idx = ImageList_AddIcon(hImageList, sfi.hIcon);
            DestroyIcon(sfi.hIcon);
            lvi.iImage = idx;

            int realIndex = ListView_InsertItem(g_hListView, &lvi);
            if (realIndex == -1) continue;

            ULARGE_INTEGER freeBytes, totalBytes, totalFree;
            std::wstring sizeStr;
            if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &totalBytes, &totalFree)) {
                wchar_t buf[128];
                swprintf_s(buf, L"%I64u GB", totalBytes.QuadPart / (1024 * 1024 * 1024));
                sizeStr = buf;
            }
            wchar_t diskType[] = L"Disk";
            wchar_t empty[] = L"";
            ListView_SetItemText(g_hListView, realIndex, 1, (LPWSTR)sizeStr.c_str());
            ListView_SetItemText(g_hListView, realIndex, 2, diskType);
            ListView_SetItemText(g_hListView, realIndex, 3, empty);

            itemIndex++;
        }
    }

    SendMessageW(g_hListView, WM_SETREDRAW, TRUE, 0);
    UpdateStatus(L"");
}

// ----------------------------------------------------------------------
// Открытие файлов / папок
// ----------------------------------------------------------------------
void OpenFile(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        NavigateTo(path, true);
        return;
    }
    ShellExecuteW(g_hWnd, L"open", path.c_str(), NULL, NULL, SW_SHOW);
}

// ----------------------------------------------------------------------
// Копирование / вставка
// ----------------------------------------------------------------------
void CopySelectedFilesToClipboard() {
    int count = ListView_GetSelectedCount(g_hListView);
    if (count == 0) return;

    std::vector<std::wstring> paths;
    int item = -1;
    while ((item = ListView_GetNextItem(g_hListView, item, LVNI_SELECTED)) != -1) {
        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_PARAM;
        lvi.iItem = item;
        if (ListView_GetItem(g_hListView, &lvi)) {
            std::wstring* pPath = (std::wstring*)lvi.lParam;
            if (pPath && *pPath != L"..") {
                paths.push_back(*pPath);
            }
        }
    }
    if (paths.empty()) return;

    size_t totalSize = sizeof(DROPFILES);
    for (const auto& p : paths) {
        totalSize += (p.length() + 1) * sizeof(wchar_t);
    }
    totalSize += sizeof(wchar_t);

    HGLOBAL hMem = GlobalAlloc(GHND, totalSize);
    if (!hMem) return;

    DROPFILES* pDrop = (DROPFILES*)GlobalLock(hMem);
    pDrop->pFiles = sizeof(DROPFILES);
    pDrop->fWide = TRUE;
    pDrop->pt.x = 0;
    pDrop->pt.y = 0;
    pDrop->fNC = FALSE;

    wchar_t* pData = (wchar_t*)((BYTE*)pDrop + sizeof(DROPFILES));
    for (const auto& p : paths) {
        wcscpy_s(pData, totalSize - ((BYTE*)pData - (BYTE*)pDrop) / sizeof(wchar_t), p.c_str());
        pData += p.length() + 1;
    }
    *pData = L'\0';

    GlobalUnlock(hMem);

    if (!OpenClipboard(g_hWnd)) {
        GlobalFree(hMem);
        return;
    }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, hMem);
    CloseClipboard();
}

void PasteFilesFromClipboard() {
    if (g_currentPath.empty()) {
        MessageBoxW(g_hWnd, L"No folder is currently open", L"Info", MB_OK);
        return;
    }

    if (!IsClipboardFormatAvailable(CF_HDROP))
        return;

    if (!OpenClipboard(g_hWnd))
        return;

    HANDLE hData = GetClipboardData(CF_HDROP);
    if (!hData) {
        CloseClipboard();
        return;
    }

    HDROP hDrop = (HDROP)hData;
    int fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    if (fileCount == 0) {
        CloseClipboard();
        return;
    }

    std::vector<std::wstring> srcPaths;
    for (int i = 0; i < fileCount; i++) {
        wchar_t buf[MAX_PATH];
        DragQueryFileW(hDrop, i, buf, MAX_PATH);
        srcPaths.push_back(buf);
    }
    CloseClipboard();

    for (const auto& srcPath : srcPaths) {
        std::wstring fileName = srcPath.substr(srcPath.find_last_of(L'\\') + 1);
        std::wstring destPath = g_currentPath + L"\\" + fileName;

        DWORD attrs = GetFileAttributesW(destPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            std::wstring newName = MakeUniqueFileName(g_currentPath, fileName);
            destPath = g_currentPath + L"\\" + newName;
        }

        if (!CopyFileW(srcPath.c_str(), destPath.c_str(), FALSE)) {
            std::wstring srcList = srcPath + L'\0' + L'\0';
            std::wstring destList = destPath + L'\0' + L'\0';
            SHFILEOPSTRUCTW op = { 0 };
            op.hwnd = g_hWnd;
            op.wFunc = FO_COPY;
            op.pFrom = srcList.c_str();
            op.pTo = destList.c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_SILENT;
            SHFileOperationW(&op);
        }
    }

    if (!g_currentPath.empty())
        PopulateListView(g_currentPath);
}

// ----------------------------------------------------------------------
// Удаление
// ----------------------------------------------------------------------
void DeleteSelectedFiles(bool permanent) {
    int count = ListView_GetSelectedCount(g_hListView);
    if (count == 0) return;

    std::vector<std::wstring> paths;
    int item = -1;
    while ((item = ListView_GetNextItem(g_hListView, item, LVNI_SELECTED)) != -1) {
        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_PARAM;
        lvi.iItem = item;
        if (ListView_GetItem(g_hListView, &lvi)) {
            std::wstring* pPath = (std::wstring*)lvi.lParam;
            if (pPath && *pPath != L"..") {
                paths.push_back(*pPath);
            }
        }
    }
    if (paths.empty()) return;

    std::wstring sourceList;
    for (const auto& p : paths) {
        sourceList += p;
        sourceList += L'\0';
    }
    sourceList += L'\0';

    SHFILEOPSTRUCTW op = { 0 };
    op.hwnd = g_hWnd;
    op.wFunc = FO_DELETE;
    op.pFrom = sourceList.c_str();
    op.fFlags = permanent ? 0 : FOF_ALLOWUNDO;
    op.fFlags |= FOF_NOCONFIRMMKDIR | FOF_SILENT;

    int result = SHFileOperationW(&op);
    if (result == 0) {
        if (!g_currentPath.empty())
            PopulateListView(g_currentPath);
    } else if (result != 1223) {
        MessageBoxW(g_hWnd, L"Delete operation failed", L"Error", MB_OK);
    }
}

// ----------------------------------------------------------------------
// Команды панели инструментов
// ----------------------------------------------------------------------
void OnToolbarCommand(int id) {
    switch (id) {
    case 1001: GoBack(); break;
    case 1002: GoForward(); break;
    case 1003: GoUp(); break;
    case 1004: EjectDrive(); break;
    case 1005:
        if (g_currentPath.empty())
            ShowDrives();
        else
            PopulateListView(g_currentPath);
        break;
    case 1006:
        PostQuitMessage(0);
        break;
    default: break;
    }
}

// ----------------------------------------------------------------------
// Подклассы
// ----------------------------------------------------------------------
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        wchar_t buf[MAX_PATH];
        GetWindowTextW(hWnd, buf, MAX_PATH);
        std::wstring path = buf;
        if (path.empty())
            NavigateTo(L"", true);
        else
            NavigateTo(path, true);
        return 0;
    }
    return CallWindowProcW(g_oldEditProc, hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK ListViewSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if (ctrl) {
            if (wParam == 'C') {
                CopySelectedFilesToClipboard();
                return 0;
            } else if (wParam == 'V') {
                PasteFilesFromClipboard();
                return 0;
            }
        } else if (wParam == VK_DELETE) {
            DeleteSelectedFiles(shift);
            return 0;
        }
    }
    return CallWindowProcW(g_oldListViewProc, hWnd, msg, wParam, lParam);
}

// ----------------------------------------------------------------------
// Главное окно
// ----------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hToolbar = CreateWindowW(TOOLBARCLASSNAMEW, NULL,
            WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_AUTOSIZE,
            0, 0, 0, 0, hWnd, NULL, GetModuleHandle(NULL), NULL);
        SendMessageW(g_hToolbar, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);

        const wchar_t* strings[] = { L"←", L"→", L"↑", L"↻", L"▲", L"✕" };
        int strIdx[6];
        for (int i = 0; i < 6; i++) {
            strIdx[i] = (int)SendMessageW(g_hToolbar, TB_ADDSTRING, 0, (LPARAM)strings[i]);
        }

        TBBUTTON tbButtons[7] = {
            {0, 1001, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE, {0,0}, 0, (INT_PTR)strIdx[0]},
            {1, 1002, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE, {0,0}, 0, (INT_PTR)strIdx[1]},
            {2, 1003, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE, {0,0}, 0, (INT_PTR)strIdx[2]},
            {0, 0, TBSTATE_ENABLED, BTNS_SEP, {0,0}, 0, 0},
            {3, 1005, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE, {0,0}, 0, (INT_PTR)strIdx[3]},
            {4, 1004, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE, {0,0}, 0, (INT_PTR)strIdx[4]},
            {5, 1006, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE, {0,0}, 0, (INT_PTR)strIdx[5]}
        };
        SendMessageW(g_hToolbar, TB_ADDBUTTONS, 7, (LPARAM)tbButtons);

        g_hAddressBar = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 200, 24, hWnd, (HMENU)1000, GetModuleHandle(NULL), NULL);
        g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_hAddressBar, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        g_hListView = CreateWindowW(WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hWnd, (HMENU)1001, GetModuleHandle(NULL), NULL);
        g_oldListViewProc = (WNDPROC)SetWindowLongPtrW(g_hListView, GWLP_WNDPROC, (LONG_PTR)ListViewSubclassProc);

        LVCOLUMNW col = { 0 };
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.cx = 200; col.pszText = (LPWSTR)L"Name"; col.iSubItem = 0;
        ListView_InsertColumn(g_hListView, 0, &col);
        col.cx = 100; col.pszText = (LPWSTR)L"Size"; col.iSubItem = 1;
        ListView_InsertColumn(g_hListView, 1, &col);
        col.cx = 150; col.pszText = (LPWSTR)L"Type"; col.iSubItem = 2;
        ListView_InsertColumn(g_hListView, 2, &col);
        col.cx = 150; col.pszText = (LPWSTR)L"Modified"; col.iSubItem = 3;
        ListView_InsertColumn(g_hListView, 3, &col);

        HIMAGELIST hImageList = ImageList_Create(16, 16, ILC_COLOR32, 10, 10);
        ListView_SetImageList(g_hListView, hImageList, LVSIL_SMALL);

        g_hStatus = CreateWindowW(STATUSCLASSNAMEW, L"",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)1002, GetModuleHandle(NULL), NULL);

        g_currentPath = L"";
        g_history.push_back(L"");
        g_historyPos = 0;
        ShowDrives();
        UpdateStatus(L"");
        g_hWnd = hWnd;
        break;
    }

    case WM_SIZE: {
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        int toolbarHeight = 40;
        int addrHeight = 24;
        int statusHeight = 24;

        if (g_hToolbar)
            SetWindowPos(g_hToolbar, NULL, 0, 0, rcClient.right, toolbarHeight, SWP_NOZORDER);
        if (g_hAddressBar)
            SetWindowPos(g_hAddressBar, NULL, 0, toolbarHeight, rcClient.right, addrHeight, SWP_NOZORDER);
        if (g_hStatus)
            SetWindowPos(g_hStatus, NULL, 0, rcClient.bottom - statusHeight, rcClient.right, statusHeight, SWP_NOZORDER);
        if (g_hListView)
            SetWindowPos(g_hListView, NULL, 0, toolbarHeight + addrHeight + 2,
                rcClient.right, rcClient.bottom - toolbarHeight - addrHeight - statusHeight - 6, SWP_NOZORDER);
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id >= 1001 && id <= 1006) {
            OnToolbarCommand(id);
        } else if (id == 2000) { // Delete
            DeleteSelectedFiles(false);
        } else if (id == 2001) { // Copy
            CopySelectedFilesToClipboard();
        } else if (id == 2002) { // Paste
            PasteFilesFromClipboard();
        } else if (id == 2003) { // Refresh
            if (g_currentPath.empty())
                ShowDrives();
            else
                PopulateListView(g_currentPath);
        } else if (id == 2004) { // Show hidden files
            g_showHidden = !g_showHidden;
            if (g_currentPath.empty())
                ShowDrives();
            else
                PopulateListView(g_currentPath);
        } else if (id == 2005) { // Eject from context menu
            int sel = ListView_GetNextItem(g_hListView, -1, LVNI_SELECTED);
            if (sel != -1) {
                LVITEMW lvi = { 0 };
                lvi.mask = LVIF_PARAM;
                lvi.iItem = sel;
                if (ListView_GetItem(g_hListView, &lvi)) {
                    std::wstring* pPath = (std::wstring*)lvi.lParam;
                    if (pPath && pPath->length() == 3 && (*pPath)[1] == L':' && (*pPath)[2] == L'\\') {
                        EjectDrive((*pPath)[0]);
                    }
                }
            }
        }
        break;
    }

    case WM_NOTIFY: {
        NMHDR* pnmh = (NMHDR*)lParam;
        if (pnmh->hwndFrom == g_hListView) {
            switch (pnmh->code) {
            case NM_DBLCLK: {
                NMITEMACTIVATE* pia = (NMITEMACTIVATE*)pnmh;
                if (pia->iItem >= 0) {
                    LVITEMW lvi = { 0 };
                    lvi.mask = LVIF_PARAM;
                    lvi.iItem = pia->iItem;
                    if (ListView_GetItem(g_hListView, &lvi)) {
                        std::wstring* pathPtr = (std::wstring*)lvi.lParam;
                        if (pathPtr) {
                            if (*pathPtr == L"..")
                                GoUp();
                            else
                                OpenFile(*pathPtr);
                        }
                    }
                }
                break;
            }
            case NM_RCLICK: {
                NMITEMACTIVATE* pia = (NMITEMACTIVATE*)pnmh;
                int idx = pia->iItem;
                bool hasSelection = (idx >= 0);

                if (hasSelection) {
                    ListView_SetItemState(g_hListView, -1, 0, LVIS_SELECTED);
                    ListView_SetItemState(g_hListView, idx, LVIS_SELECTED, LVIS_SELECTED);
                }

                bool canPaste = false;
                if (!g_currentPath.empty() && IsClipboardFormatAvailable(CF_HDROP)) {
                    canPaste = true;
                }

                HMENU hMenu = CreatePopupMenu();

                bool isDriveRoot = false;
                bool canEject = false;
                if (hasSelection) {
                    LVITEMW lvi = { 0 };
                    lvi.mask = LVIF_PARAM;
                    lvi.iItem = idx;
                    if (ListView_GetItem(g_hListView, &lvi)) {
                        std::wstring* pPath = (std::wstring*)lvi.lParam;
                        if (pPath && pPath->length() == 3 && (*pPath)[1] == L':' && (*pPath)[2] == L'\\') {
                            isDriveRoot = true;
                            UINT type = GetDriveTypeW(pPath->c_str());
                            if (type == DRIVE_REMOVABLE || type == DRIVE_CDROM) {
                                canEject = true;
                            }
                        }
                    }
                }

                if (hasSelection) {
                    AppendMenuW(hMenu, MF_STRING, 2001, L"Copy");
                    AppendMenuW(hMenu, MF_STRING | (canPaste ? MF_ENABLED : MF_GRAYED), 2002, L"Paste");
                    AppendMenuW(hMenu, MF_STRING, 2000, L"Delete");
                    if (canEject) {
                        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                        AppendMenuW(hMenu, MF_STRING, 2005, L"Eject");
                    }
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenuW(hMenu, MF_STRING, 2003, L"Refresh");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenuW(hMenu, MF_STRING | (g_showHidden ? MF_CHECKED : MF_UNCHECKED), 2004, L"Show hidden files");
                } else {
                    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 2001, L"Copy");
                    AppendMenuW(hMenu, MF_STRING | (canPaste ? MF_ENABLED : MF_GRAYED), 2002, L"Paste");
                    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 2000, L"Delete");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenuW(hMenu, MF_STRING, 2003, L"Refresh");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenuW(hMenu, MF_STRING | (g_showHidden ? MF_CHECKED : MF_UNCHECKED), 2004, L"Show hidden files");
                }

                POINT pt;
                GetCursorPos(&pt);
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
                DestroyMenu(hMenu);
                break;
            }
            }
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ----------------------------------------------------------------------
// Точка входа
// ----------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"FileManagerClass";
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Window class registration failed", L"Error", MB_OK);
        return 1;
    }

    HWND hWnd = CreateWindowExW(0, L"FileManagerClass", L"File Manager",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInstance, NULL);
    if (!hWnd) {
        MessageBoxW(NULL, L"Window creation failed", L"Error", MB_OK);
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
