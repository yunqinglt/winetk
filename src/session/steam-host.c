/* Keep the Windows Steam presence alive while Proton's lsteamclient talks to
 * the user's running Linux Steam client. No account or entitlement emulation. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child;
    DWORD pid = GetCurrentProcessId(), code = 1;
    HANDLE event;
    HMODULE client;
    wchar_t *command;
    size_t length = 1;
    if (argc < 2) return 2;
    for (int i = 1; i < argc; ++i) length += 2*wcslen(argv[i])+4;
    command = calloc(length,sizeof(*command));
    if (!command) return 1;
    /* Quote Windows argv, including backslashes before quotes or closing quote. */
    wchar_t *out = command;
    for (int i = 1; i < argc; ++i)
    {
        const wchar_t *in = argv[i];
        if (i > 1) *out++ = L' ';
        *out++ = L'"';
        while (*in)
        {
            unsigned slashes = 0;
            while (*in == L'\\') { ++slashes; ++in; }
            unsigned count = (*in == L'"' || !*in) ? 2*slashes : slashes;
            while (count--) *out++ = L'\\';
            if (*in == L'"') *out++ = L'\\';
            if (*in) *out++ = *in++;
        }
        *out++ = L'"';
    }
    *out = 0;
    event = CreateEventW(NULL,FALSE,FALSE,L"Steam3Master_SharedMemLock");
    client = LoadLibraryW(L"lsteamclient.dll");
    if (!client) { fprintf(stderr,"Steam bridge load failed: %lu\n",GetLastError()); goto done; }
    RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam\\ActiveProcess",L"pid",REG_DWORD,&pid,sizeof(pid));
    const wchar_t dll[] = L"C:\\windows\\system32\\steamclient64.dll";
    const wchar_t dll32[] = L"C:\\windows\\syswow64\\steamclient.dll";
    RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam\\ActiveProcess",L"SteamClientDll64",REG_SZ,dll,sizeof(dll));
    RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam\\ActiveProcess",L"SteamClientDll",REG_SZ,dll32,sizeof(dll32));
    if (!CreateProcessW(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&child))
        fprintf(stderr,"Steam game launch failed: %lu\n",GetLastError());
    else
    {
        CloseHandle(child.hThread);
        WaitForSingleObject(child.hProcess,INFINITE);
        GetExitCodeProcess(child.hProcess,&code);
        CloseHandle(child.hProcess);
    }
done:
    if (event) CloseHandle(event);
    if (client) FreeLibrary(client);
    free(command);
    return code;
}
