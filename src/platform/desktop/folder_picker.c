#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_install.h"

int gex_default_game_folder(char *path, size_t size) {
    int n = snprintf(path, size, "Gex");
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>

int gex_pick_game_folder(char *path, size_t size, char *error, size_t error_size) {
    if (MessageBoxW(NULL,
            L"Select the folder with the installation of the PC version of Gex from GOG.",
            L"Select the Gex installation folder", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
        return 1;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HRESULT result = initialized;
    IFileOpenDialog *dialog = NULL;
    IShellItem *folder = NULL;
    PWSTR selected = NULL;
    if (SUCCEEDED(result))
        result = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileOpenDialog, (void **)&dialog);
    if (SUCCEEDED(result))
        result = IFileOpenDialog_SetOptions(dialog,
            FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    if (SUCCEEDED(result))
        result = IFileOpenDialog_SetTitle(dialog,
            L"Select the Gex installation folder");
    if (SUCCEEDED(result)) result = IFileOpenDialog_Show(dialog, NULL);
    int cancelled = result == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (SUCCEEDED(result)) result = IFileOpenDialog_GetResult(dialog, &folder);
    if (SUCCEEDED(result)) result = IShellItem_GetDisplayName(folder, SIGDN_FILESYSPATH, &selected);
    if (SUCCEEDED(result) &&
        (size > INT_MAX || !WideCharToMultiByte(CP_UTF8, 0, selected, -1,
                                                path, (int)size, NULL, NULL)))
        result = HRESULT_FROM_WIN32(GetLastError());
    if (selected) CoTaskMemFree(selected);
    if (folder) IShellItem_Release(folder);
    if (dialog) IFileOpenDialog_Release(dialog);
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (cancelled) return 1;
    if (FAILED(result)) {
        snprintf(error, error_size, "Cannot open the game folder picker (Windows error 0x%08lX). "
                 "You can also supply --data PATH.", (unsigned long)result);
        return -1;
    }
    return 0;
}

void gex_setup_error(const char *message) {
    wchar_t text[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, message, -1, text, sizeof text / sizeof *text))
        wcscpy(text, L"Game import failed. See the application log.");
    MessageBoxW(NULL, text, L"Gex setup", MB_OK | MB_ICONERROR);
}
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int execute(char *const arguments[], char *output, size_t size) {
    int descriptors[2];
    if (pipe(descriptors)) return -1;
    pid_t child = fork();
    if (child < 0) { close(descriptors[0]); close(descriptors[1]); return -1; }
    if (!child) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(126);
        close(descriptors[1]);
        execvp(arguments[0], arguments);
        _exit(errno == ENOENT ? 127 : 126);
    }
    close(descriptors[1]);
    size_t used = 0;
    int failed = 0;
    char buffer[1024];
    for (;;) {
        ssize_t count = read(descriptors[0], buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { failed = 1; break; }
        if (!count) break;
        if ((size_t)count >= size - used) failed = 1;
        else if (!failed) { memcpy(output + used, buffer, (size_t)count); used += (size_t)count; }
    }
    close(descriptors[0]);
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || failed || !WIFEXITED(status)) return -1;
    output[used] = '\0';
    if (used && output[used - 1] == '\n') output[--used] = '\0';
    return WEXITSTATUS(status);
}

int gex_pick_game_folder(char *path, size_t size, char *error, size_t error_size) {
    char *notice[] = {"zenity", "--info", "--no-markup", "--ok-label=Continue",
        "--title=Select the Gex installation folder", "--text",
        "Select the folder with the installation of the PC version of Gex from GOG.", NULL};
    int result = execute(notice, path, size);
    if (result == 127) {
        char *alternative[] = {"kdialog", "--msgbox",
            "Select the folder with the installation of the PC version of Gex from GOG.",
            "--title", "Select the Gex installation folder", NULL};
        result = execute(alternative, path, size);
    }
    if (result == 1) return 1;
    if (result) {
        snprintf(error, error_size, "Cannot open the folder picker instructions. "
                 "Install zenity or kdialog and run from a desktop session, or supply --data PATH.");
        return -1;
    }
    char *arguments[] = {"zenity", "--file-selection", "--directory",
        "--title=Select the Gex installation folder", NULL};
    result = execute(arguments, path, size);
    if (result == 127) {
        char *alternative[] = {"kdialog", "--getexistingdirectory", ".",
            "--title", "Select the Gex installation folder", NULL};
        result = execute(alternative, path, size);
    }
    if (result == 1) return 1;
    if (result || !*path) {
        snprintf(error, error_size,
            "Cannot open the folder picker. Install zenity or kdialog and run from a desktop session, "
            "or supply --data PATH."
        );
        return -1;
    }
    return 0;
}

void gex_setup_error(const char *message) {
    char output[1024];
    char *arguments[] = {"zenity", "--error", "--no-markup", "--title=Gex setup",
        "--text", (char *)message, NULL};
    int result = execute(arguments, output, sizeof output);
    if (result == 127) {
        char *alternative[] = {"kdialog", "--error", (char *)message, "--title", "Gex setup", NULL};
        result = execute(alternative, output, sizeof output);
    }
    if (result) fprintf(stderr, "[setup] cannot display the setup error dialog (%d)\n", result);
}
#endif
