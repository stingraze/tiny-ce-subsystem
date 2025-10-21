// wslce_tiny.c
// Ultra-small "WSL-ish" for Windows CE: POSIX-lite shim + tiny shell.
// CeGCC (arm-mingw32ce-gcc) build:  arm-mingw32ce-gcc -Os -s -Wl,--subsystem,windows -o wslce.exe wslce_tiny.c -lcoredll
// No GDI text draws; we use an EDIT control for I/O. Minimal deps: coredll.dll.

#define _WIN32_WCE 0x0501
#define WINVER      0x0501

#include <windows.h>
#include <winbase.h>
#include <commdlg.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

// -----------------------------
// Globals / Config
// -----------------------------
static HWND g_hwnd = NULL;
static HWND g_edit = NULL;
static WCHAR g_title[] = L"WSL-CE Tiny (cesh)";
static WCHAR g_class[] = L"WSLCE_TINY_CLASS";
static char  g_root_utf8[512] = {0}; // UTF-8 root for "/"
static char  g_cwd_utf8[1024] = "/"; // virtual cwd

// -----------------------------
// Utilities: UTF-8 <-> UTF-16
// -----------------------------
static void utf8_to_utf16(const char* s, WCHAR* wbuf, int wcap) {
    if (!s) { wbuf[0] = 0; return; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, wbuf, wcap);
}

static void utf16_to_utf8(const WCHAR* ws, char* buf, int cap) {
    if (!ws) { if (cap) buf[0] = 0; return; }
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, buf, cap, NULL, NULL);
}

// -----------------------------
// Console I/O (EDIT control)
// -----------------------------
static void con_append_w(const WCHAR* ws) {
    // append to edit control without GDI
    if (!g_edit) return;
    DWORD len = GetWindowTextLengthW(g_edit);
    SendMessageW(g_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_edit, EM_REPLACESEL, (WPARAM)FALSE, (LPARAM)ws);
}

static void con_print(const char* fmt, ...) {
    char tmp[2048];
    va_list ap; va_start(ap, fmt);
    wvsnprintfA(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    WCHAR w[4096];
    utf8_to_utf16(tmp, w, 4096);
    con_append_w(w);
}

static void con_println(const char* fmt, ...) {
    char tmp[2048];
    va_list ap; va_start(ap, fmt);
    wvsnprintfA(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    lstrcatA(tmp, "\r\n");
    WCHAR w[4096];
    utf8_to_utf16(tmp, w, 4096);
    con_append_w(w);
}

static void prompt() {
    con_print("%s $ ", g_cwd_utf8);
}

// -----------------------------
// Path translation
// Linux-ish path -> WinCE absolute
// -----------------------------
static void ensure_default_root() {
    if (g_root_utf8[0]) return;
    // Try env var first (if present)
    char envroot[512] = {0};
    DWORD got = GetEnvironmentVariableA("WSLCE_ROOT", envroot, sizeof(envroot));
    if (got > 0 && got < sizeof(envroot)) {
        lstrcpynA(g_root_utf8, envroot, sizeof(g_root_utf8));
        return;
    }
    // Fallback default
    lstrcpynA(g_root_utf8, "\\Storage Card\\wslce-root", sizeof(g_root_utf8));
}

// Join two WinCE parts with backslash (no normalization beyond that)
static void join_wince_path(const char* a, const char* b, char* out, int cap) {
    lstrcpynA(out, a, cap);
    int la = lstrlenA(out);
    if (la > 0 && out[la-1] != '\\') {
        if (la + 1 < cap) out[la++] = '\\';
        out[la] = 0;
    }
    lstrcpynA(out + la, b, cap - la);
}

// Normalize incoming linux path into absolute virtual path starting with '/'
static void normalize_linux_path(const char* in, char* out, int cap) {
    if (!in || !in[0]) { lstrcpynA(out, g_cwd_utf8, cap); return; }
    if (in[0] == '/') { lstrcpynA(out, in, cap); return; }
    // relative -> cwd + "/" + in
    lstrcpynA(out, g_cwd_utf8, cap);
    int l = lstrlenA(out);
    if (l > 1 && out[l-1] != '/') {
        if (l + 1 < cap) out[l++] = '/';
        out[l] = 0;
    }
    lstrcpynA(out + l, in, cap - l);
}

// Translate Linux path -> WinCE absolute path under g_root
static void linux_to_wince_path(const char* linuxPath, char* wincePath, int cap) {
    ensure_default_root();
    // strip leading '/'
    const char* p = linuxPath;
    while (*p == '/') ++p;

    // replace '/' with '\'
    char rel[1024]; int idx = 0;
    for (; *p && idx < (int)sizeof(rel)-1; ++p) {
        rel[idx++] = (*p == '/') ? '\\' : *p;
    }
    rel[idx] = 0;

    if (rel[0]) join_wince_path(g_root_utf8, rel, wincePath, cap);
    else lstrcpynA(wincePath, g_root_utf8, cap);
}

// -----------------------------
// Minimal POSIX-like wrappers
// -----------------------------
#define MAX_FD  32
static HANDLE g_fdtab[MAX_FD] = {0};

static int fd_alloc(HANDLE h) {
    for (int i=3; i<MAX_FD; ++i) { // 0,1,2 reserved
        if (g_fdtab[i] == 0) { g_fdtab[i] = h; return i; }
    }
    return -1;
}
static HANDLE fd_get(int fd) {
    if (fd < 0 || fd >= MAX_FD) return INVALID_HANDLE_VALUE;
    return g_fdtab[fd] ? g_fdtab[fd] : INVALID_HANDLE_VALUE;
}
static void fd_free(int fd) {
    if (fd >= 0 && fd < MAX_FD) g_fdtab[fd] = 0;
}

static DWORD map_oflags(DWORD oflags) {
    // map O_* to WinCE access modes (simple subset)
    // Assume: 0=RDONLY, 1=WRONLY, 2=RDWR, 0x40=O_CREAT, 0x200=O_TRUNC, 0x400=O_APPEND
    DWORD acc = GENERIC_READ;
    if ((oflags & 3) == 1) acc = GENERIC_WRITE;
    else if ((oflags & 3) == 2) acc = GENERIC_READ | GENERIC_WRITE;
    else acc = GENERIC_READ;

    return acc;
}
static DWORD map_creation(DWORD oflags) {
    DWORD disp = OPEN_EXISTING;
    if (oflags & 0x40) { // O_CREAT
        if (oflags & 0x200) disp = CREATE_ALWAYS; // O_TRUNC
        else disp = OPEN_ALWAYS;
    } else if (oflags & 0x200) {
        disp = TRUNCATE_EXISTING;
    }
    return disp;
}

static int ce_open(const char* path, int oflags, int mode) {
    char wince[1024];
    linux_to_wince_path(path, wince, sizeof(wince));
    WCHAR wpath[1024];
    utf8_to_utf16(wince, wpath, 1024);

    DWORD acc = map_oflags(oflags);
    DWORD disp = map_creation(oflags);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    HANDLE h = CreateFileW(wpath, acc, share, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    int fd = fd_alloc(h);
    if (fd < 0) { CloseHandle(h); return -1; }

    if (oflags & 0x400) { // O_APPEND
        SetFilePointer(h, 0, NULL, FILE_END);
    }
    return fd;
}

static int ce_close(int fd) {
    HANDLE h = fd_get(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(h);
    fd_free(fd);
    return 0;
}

static int ce_read(int fd, void* buf, unsigned len) {
    HANDLE h = fd_get(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    if (!ReadFile(h, buf, len, &got, NULL)) return -1;
    return (int)got;
}

static int ce_write(int fd, const void* buf, unsigned len) {
    HANDLE h = fd_get(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD put = 0;
    if (!WriteFile(h, buf, len, &put, NULL)) return -1;
    return (int)put;
}

struct dirent {
    char d_name[260];
    int  d_type; // 4=dir, 8=file (POSIX-ish hints)
};

typedef struct {
    HANDLE hFind;
    WIN32_FIND_DATAW wfd;
    char pattern[1024];
    int first;
} DIR;

static DIR* ce_opendir(const char* path) {
    char wince[1024], pat[1024];
    linux_to_wince_path(path, wince, sizeof(wince));
    // append \*
    lstrcpynA(pat, wince, sizeof(pat));
    int l = lstrlenA(pat);
    if (l > 0 && pat[l-1] != '\\') { pat[l++]='\\'; pat[l]=0; }
    lstrcpynA(pat + l, "*", sizeof(pat) - l);

    WCHAR wpat[1024];
    utf8_to_utf16(pat, wpat, 1024);
    HANDLE h = FindFirstFileW(wpat, &((WIN32_FIND_DATAW){0}));
    if (h == INVALID_HANDLE_VALUE) {
        // we need the data; re-open properly
    }

    DIR* d = (DIR*)LocalAlloc(LPTR, sizeof(DIR));
    if (!d) return NULL;

    // Actual first call:
    d->hFind = FindFirstFileW(wpat, &d->wfd);
    if (d->hFind == INVALID_HANDLE_VALUE) { LocalFree(d); return NULL; }
    lstrcpynA(d->pattern, pat, sizeof(d->pattern));
    d->first = 1;
    return d;
}

static struct dirent* ce_readdir(DIR* d) {
    static struct dirent e;
    if (!d) return NULL;
    BOOL ok;
    if (d->first) { ok = TRUE; d->first = 0; }
    else ok = FindNextFileW(d->hFind, &d->wfd);
    if (!ok) return NULL;

    utf16_to_utf8(d->wfd.cFileName, e.d_name, sizeof(e.d_name));
    if (d->wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) e.d_type = 4;
    else e.d_type = 8;
    return &e;
}

static int ce_closedir(DIR* d) {
    if (!d) return -1;
    if (d->hFind != INVALID_HANDLE_VALUE) FindClose(d->hFind);
    LocalFree(d);
    return 0;
}

static int ce_mkdir(const char* path) {
    char wince[1024];
    linux_to_wince_path(path, wince, sizeof(wince));
    WCHAR w[1024]; utf8_to_utf16(wince, w, 1024);
    return CreateDirectoryW(w, NULL) ? 0 : -1;
}

static int ce_rmdir(const char* path) {
    char wince[1024];
    linux_to_wince_path(path, wince, sizeof(wince));
    WCHAR w[1024]; utf8_to_utf16(wince, w, 1024);
    return RemoveDirectoryW(w) ? 0 : -1;
}

static int ce_unlink(const char* path) {
    char wince[1024];
    linux_to_wince_path(path, wince, sizeof(wince));
    WCHAR w[1024]; utf8_to_utf16(wince, w, 1024);
    return DeleteFileW(w) ? 0 : -1;
}

static int ce_rename(const char* a, const char* b) {
    char wa[1024], wb[1024];
    linux_to_wince_path(a, wa, sizeof(wa));
    linux_to_wince_path(b, wb, sizeof(wb));
    WCHAR A[1024], B[1024]; utf8_to_utf16(wa, A, 1024); utf8_to_utf16(wb, B, 1024);
    return MoveFileW(A, B) ? 0 : -1;
}

static int ce_getcwd(char* buf, int cap) {
    lstrcpynA(buf, g_cwd_utf8, cap);
    return 0;
}

static int ce_chdir(const char* path) {
    char norm[1024];
    normalize_linux_path(path, norm, sizeof(norm));
    // Check it exists and is dir
    char wince[1024]; linux_to_wince_path(norm, wince, sizeof(wince));
    WCHAR w[1024]; utf8_to_utf16(wince, w, 1024);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h==INVALID_HANDLE_VALUE) return -1;
    BOOL isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)!=0;
    FindClose(h);
    if (!isDir) return -1;
    // Normalize by removing trailing '/'
    if (norm[0]==0) lstrcpynA(norm, "/", sizeof(norm));
    int l = lstrlenA(norm);
    if (l>1 && norm[l-1]=='/') norm[l-1]=0;
    lstrcpynA(g_cwd_utf8, norm, sizeof(g_cwd_utf8));
    return 0;
}

// -----------------------------
// File helpers for built-ins
// -----------------------------
static int copy_file(const char* src, const char* dst) {
    int sfd = ce_open(src, 0/*RDONLY*/, 0);
    if (sfd<0) return -1;
    int dfd = ce_open(dst, 0x41/*WRONLY|O_CREAT*/, 0644);
    if (dfd<0) { ce_close(sfd); return -1; }
    char buf[2048]; int n;
    while ((n = ce_read(sfd, buf, sizeof(buf))) > 0) {
        if (ce_write(dfd, buf, (unsigned)n) != n) { ce_close(sfd); ce_close(dfd); return -1; }
    }
    ce_close(sfd); ce_close(dfd);
    return 0;
}

// -----------------------------
// Process spawn (for WinCE EXEs)
// -----------------------------
static int ce_spawn(const char* winceAbsExePath, const char* cmdlineUtf8) {
    // We accept absolute WinCE path (e.g., \Windows\calc.exe) or translated Linux path.
    WCHAR wexe[1024]; utf8_to_utf16(winceAbsExePath, wexe, 1024);
    WCHAR wcmd[1024]; utf8_to_utf16(cmdlineUtf8?cmdlineUtf8:"", wcmd, 1024);

    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    BOOL ok = CreateProcessW(wexe, wcmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (!ok) return -1;
    CloseHandle(pi.hThread);
    // Wait (synchronously) for completion
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    return 0;
}

// -----------------------------
// Tiny shell / parser
// -----------------------------
#define MAX_TOK 32
static int tokenize(char* line, char* argv[], int maxv) {
    int argc = 0;
    char* p = line;
    while (*p && argc < maxv) {
        while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') ++p;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p!=' ' && *p!='\t' && *p!='\r' && *p!='\n') ++p;
        if (*p) { *p=0; ++p; }
    }
    return argc;
}

static void bi_help() {
    con_println("Built-ins:");
    con_println("  help                 - this help");
    con_println("  pwd                  - print cwd");
    con_println("  cd <dir>             - change directory");
    con_println("  ls [path]            - list directory");
    con_println("  cat <file>           - print file");
    con_println("  echo [args...]       - echo");
    con_println("  touch <file>         - create empty file");
    con_println("  mkdir <dir>          - make directory");
    con_println("  rmdir <dir>          - remove directory");
    con_println("  rm <file>            - remove file");
    con_println("  mv <src> <dst>       - rename/move");
    con_println("  cp <src> <dst>       - copy file");
    con_println("  hexdump <file>       - hex dump");
    con_println("  run <abs-winCE-exe> [args...] - spawn WinCE EXE");
    con_println("  setroot <\\CE\\path>  - set WinCE root for '/'");
    con_println("  exit                 - quit");
}

static void bi_pwd() {
    con_println("%s", g_cwd_utf8);
}
static void bi_cd(int argc, char** argv) {
    const char* t = (argc>1)? argv[1] : "/";
    if (ce_chdir(t) == 0) { /* ok */ }
    else con_println("cd: no such directory: %s", t);
}
static void bi_ls(int argc, char** argv) {
    const char* t = (argc>1)? argv[1] : ".";
    DIR* d = ce_opendir(t);
    if (!d) { con_println("ls: cannot open: %s", t); return; }
    struct dirent* e;
    while ((e = ce_readdir(d)) != NULL) {
        if (lstrcmpA(e->d_name, ".")==0 || lstrcmpA(e->d_name, "..")==0) continue;
        con_println("%s%s", e->d_name, (e->d_type==4?"/":""));
    }
    ce_closedir(d);
}
static void bi_cat(int argc, char** argv) {
    if (argc<2) { con_println("cat: missing file"); return; }
    int fd = ce_open(argv[1], 0, 0);
    if (fd<0) { con_println("cat: cannot open: %s", argv[1]); return; }
    char buf[1024]; int n;
    while ((n=ce_read(fd, buf, sizeof(buf)))>0) {
        // ensure it is printable; just pass-through
        char tmp[1025]; if (n>1024) n=1024; memcpy(tmp, buf, n); tmp[n]=0;
        con_print("%s", tmp);
    }
    ce_close(fd);
    con_println("");
}
static void bi_echo(int argc, char** argv) {
    for (int i=1;i<argc;i++) {
        con_print("%s", argv[i]);
        if (i+1<argc) con_print(" ");
    }
    con_println("");
}
static void bi_touch(int argc, char** argv) {
    if (argc<2) { con_println("touch: missing file"); return; }
    int fd = ce_open(argv[1], 0x40/*O_CREAT*/, 0644);
    if (fd<0) { con_println("touch: cannot create: %s", argv[1]); return; }
    ce_close(fd);
}
static void bi_mkdir(int argc, char** argv) {
    if (argc<2) { con_println("mkdir: missing dir"); return; }
    if (ce_mkdir(argv[1])<0) con_println("mkdir: failed: %s", argv[1]);
}
static void bi_rmdir(int argc, char** argv) {
    if (argc<2) { con_println("rmdir: missing dir"); return; }
    if (ce_rmdir(argv[1])<0) con_println("rmdir: failed: %s", argv[1]);
}
static void bi_rm(int argc, char** argv) {
    if (argc<2) { con_println("rm: missing file"); return; }
    if (ce_unlink(argv[1])<0) con_println("rm: failed: %s", argv[1]);
}
static void bi_mv(int argc, char** argv) {
    if (argc<3) { con_println("mv: src dst"); return; }
    if (ce_rename(argv[1], argv[2])<0) con_println("mv: failed");
}
static void bi_cp(int argc, char** argv) {
    if (argc<3) { con_println("cp: src dst"); return; }
    if (copy_file(argv[1], argv[2])<0) con_println("cp: failed");
}
static void bi_hexdump(int argc, char** argv) {
    if (argc<2) { con_println("hexdump: file"); return; }
    int fd = ce_open(argv[1], 0, 0);
    if (fd<0) { con_println("hexdump: cannot open"); return; }
    unsigned char b[16]; int n; unsigned long off=0;
    while ((n=ce_read(fd,b,16))>0) {
        con_print("%08lx  ", off);
        for (int i=0;i<16;i++){
            if (i<n) con_print("%02x ", b[i]); else con_print("   ");
        }
        con_print(" |");
        for (int i=0;i<n;i++){
            char c = (b[i]>=32 && b[i]<127)? (char)b[i] : '.';
            char s[2]={c,0}; con_print("%s", s);
        }
        con_println("|");
        off += (unsigned)n;
    }
    ce_close(fd);
}
static void bi_run(int argc, char** argv) {
    if (argc<2) { con_println("run: <\\winCE\\abs\\exe> [args]"); return; }
    // If it looks like a linux path, translate first.
    char exe[1024];
    if (argv[1][0]=='/') {
        linux_to_wince_path(argv[1], exe, sizeof(exe));
    } else {
        lstrcpynA(exe, argv[1], sizeof(exe));
    }
    // Build arg string (after exe)
    char cmd[512]="";
    for (int i=2;i<argc;i++){
        lstrcatA(cmd, argv[i]);
        if (i+1<argc) lstrcatA(cmd, " ");
    }
    int rc = ce_spawn(exe, cmd[0]?cmd:NULL);
    if (rc<0) con_println("run: failed");
}

static void bi_setroot(int argc, char** argv) {
    if (argc<2) { con_println("setroot: <\\CE\\path>"); return; }
    lstrcpynA(g_root_utf8, argv[1], sizeof(g_root_utf8));
    con_println("root now: %s", g_root_utf8);
}

// Return 1 to exit
static int exec_line(char* line) {
    // strip CRLF
    for (char* p=line; *p; ++p) if (*p=='\r'||*p=='\n') *p=0;
    if (!line[0]) return 0;

    char* argv[MAX_TOK]={0};
    int argc = tokenize(line, argv, MAX_TOK);
    if (argc==0) return 0;

    if (lstrcmpA(argv[0], "help")==0) bi_help();
    else if (lstrcmpA(argv[0], "pwd")==0) bi_pwd();
    else if (lstrcmpA(argv[0], "cd")==0) bi_cd(argc, argv);
    else if (lstrcmpA(argv[0], "ls")==0) bi_ls(argc, argv);
    else if (lstrcmpA(argv[0], "cat")==0) bi_cat(argc, argv);
    else if (lstrcmpA(argv[0], "echo")==0) bi_echo(argc, argv);
    else if (lstrcmpA(argv[0], "touch")==0) bi_touch(argc, argv);
    else if (lstrcmpA(argv[0], "mkdir")==0) bi_mkdir(argc, argv);
    else if (lstrcmpA(argv[0], "rmdir")==0) bi_rmdir(argc, argv);
    else if (lstrcmpA(argv[0], "rm")==0) bi_rm(argc, argv);
    else if (lstrcmpA(argv[0], "mv")==0) bi_mv(argc, argv);
    else if (lstrcmpA(argv[0], "cp")==0) bi_cp(argc, argv);
    else if (lstrcmpA(argv[0], "hexdump")==0) bi_hexdump(argc, argv);
    else if (lstrcmpA(argv[0], "run")==0) bi_run(argc, argv);
    else if (lstrcmpA(argv[0], "setroot")==0) bi_setroot(argc, argv);
    else if (lstrcmpA(argv[0], "exit")==0) return 1;
    else con_println("%s: not found (built-in only)", argv[0]);

    return 0;
}

// -----------------------------
// GUI boilerplate (EDIT console)
// -----------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_edit = CreateWindowW(L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|ES_READONLY,
            0,0,0,0, h, (HMENU)100, GetModuleHandle(NULL), NULL);
        SendMessageW(g_edit, EM_SETLIMITTEXT, 0, 0); // unlimited-ish
        con_println("Welcome to WSL-CE Tiny.");
        ensure_default_root();
        con_println("Root: %s", g_root_utf8);
        prompt();
    } break;
    case WM_SIZE:
        if (g_edit) MoveWindow(g_edit, 0, 0, LOWORD(l), HIWORD(l), TRUE);
        return 0;
    case WM_KEYDOWN:
        // handle Enter by reading from an input buffer? We made EDIT read-only.
        // Simpler: capture Enter globally and pop up an input dialog each time.
        if (w == VK_RETURN) {
            // Get a small modal input to keep dependencies zero
            WCHAR inw[512]=L"";
            // Minimal input prompt via InputBox is not standard on CE;
            // fallback to a tiny custom dialog-less prompt: use a simple MessageBox won't capture input.
            // So: temporarily make edit writable and grab last line.
            SendMessageW(g_edit, EM_SETREADONLY, FALSE, 0);
            // Append newline to move caret
            con_append_w(L"\r\n");
            // Ask user by a minimal trick: prepend prompt and let user type after it.
            // For tiny dependencies, we implement a very basic input line atop a modal dialog substitute:
            // In practice: we toggle to writable and let user type, on next Enter we take last line.
        }
        break;
    case WM_CHAR:
        if (w == L'\r') {
            // Read last line from edit control
            int len = GetWindowTextLengthW(g_edit);
            WCHAR* all = (WCHAR*)LocalAlloc(LPTR, (len+2)*sizeof(WCHAR));
            if (!all) break;
            GetWindowTextW(g_edit, all, len+1);
            // find last '\n'
            int i = len-1;
            while (i>=0 && all[i] != L'\n') --i;
            int start = (i<0)? 0 : (i+1);
            // extract line (remove \r if any)
            WCHAR linew[512]; int k=0;
            for (int j=start; j<len && k<511; ++j) {
                WCHAR c = all[j];
                if (c==L'\r' || c==L'\n') break;
                linew[k++] = c;
            }
            linew[k]=0;
            LocalFree(all);

            char line8[1024];
            utf16_to_utf8(linew, line8, sizeof(line8));

            // Now process it
            char buf[1024]; lstrcpynA(buf, line8, sizeof(buf));
            int want_exit = exec_line(buf);

            // Restore read-only and print next prompt
            if (want_exit) {
                PostQuitMessage(0);
            } else {
                // make editable to insert \r\n and prompt, then back to read-only
                SendMessageW(g_edit, EM_SETREADONLY, FALSE, 0);
                con_append_w(L"\r\n");
                SendMessageW(g_edit, EM_SETREADONLY, TRUE, 0);
                prompt();
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPWSTR cmd, int show) {
    // init cwd "/"
    lstrcpynA(g_cwd_utf8, "/", sizeof(g_cwd_utf8));

    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = h;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = g_class;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowW(g_class, g_title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 640, NULL, NULL, h, NULL);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
