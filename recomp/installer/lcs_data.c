/* lcs-data: copies the game data of the user's own installed GTA: Liberty City Stories (Android) into the
 * layout lcs.exe expects:  <dest>/lib/{libGame,libopenal,libVendor_mpg123}.so  and  <dest>/assets/...
 *
 *   lcs-data --dest <install>\data                 pull from a phone over ADB (USB debugging)
 *   lcs-data --dest <dir> --apks <folder> [--obb F] use APK files (and an old OBB) already on this PC
 *
 * APKs are zip files; they are unpacked with Windows' own tar.exe. The radio stations are not in the current
 * Android release: when the old main.*.obb is present, its MP3 entries that the new data_main.wad lacks are
 * copied into data_music.wad (same encrypted payloads; port of recomp/tools/extract_radio_wad.py). */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <direct.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PKG "com.rockstargames.gtalcs"
#define PLATFORM_TOOLS "https://dl.google.com/android/repository/platform-tools-latest-windows.zip"

static char adb[MAX_PATH * 2], stage[MAX_PATH], dest[MAX_PATH];
static int pause_at_end = 1;

static void finish(int code) {
    if (pause_at_end && code) { printf("\nPress Enter to close this window."); getchar(); }
    exit(code);
}
static void fail(const char *fmt, const char *arg) {
    printf("\nERROR: "); printf(fmt, arg); printf("\n");
    finish(1);
}
static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static int run(const char *cmd) { /* cmd /c needs the whole line quoted when it starts with a quoted path */
    char line[4096]; snprintf(line, sizeof line, "\"%s\"", cmd);
    return system(line);
}
static int capture(const char *cmd, char *out, size_t n) {
    char line[4096]; snprintf(line, sizeof line, "\"%s\"", cmd);
    FILE *f = _popen(line, "r");
    size_t used = 0; out[0] = 0;
    if (!f) return -1;
    while (used + 1 < n && fgets(out + used, (int)(n - used), f)) used += strlen(out + used);
    return _pclose(f);
}
static void mkdirs(const char *path) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 3; *s; s++) if (*s == '\\' || *s == '/') { char c = *s; *s = 0; _mkdir(p); *s = c; }
    _mkdir(p);
}
static void remove_tree(const char *path) {
    char pattern[MAX_PATH]; WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char child[MAX_PATH]; snprintf(child, sizeof child, "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else { SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL); DeleteFileA(child); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
}
/* move src's contents into dst, replacing files that already exist */
static void merge_tree(const char *src, const char *dst) {
    char pattern[MAX_PATH]; WIN32_FIND_DATAA fd;
    mkdirs(dst);
    snprintf(pattern, sizeof pattern, "%s\\*", src);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char a[MAX_PATH], b[MAX_PATH];
        snprintf(a, sizeof a, "%s\\%s", src, fd.cFileName); snprintf(b, sizeof b, "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) merge_tree(a, b);
        else if (!MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) fail("cannot move %s", a);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* ---- ADB ---------------------------------------------------------------------------------------------- */
static void find_adb(void) {
    char out[1024];
    if (adb[0]) return;
    char exe[MAX_PATH]; GetModuleFileNameA(0, exe, sizeof exe);
    char *p = strrchr(exe, '\\'); if (p) *p = 0;
    snprintf(adb, sizeof adb, "%s\\platform-tools\\adb.exe", exe);
    if (exists(adb)) return;
    if (capture("where adb.exe 2>nul", out, sizeof out) == 0 && out[0]) {
        char *nl = strpbrk(out, "\r\n"); if (nl) *nl = 0;
        snprintf(adb, sizeof adb, "%s", out);
        return;
    }
    printf("ADB (Android platform-tools) was not found. Downloading it from Google:\n  %s\n", PLATFORM_TOOLS);
    char zip[MAX_PATH], cmd[2048];
    snprintf(zip, sizeof zip, "%s\\platform-tools.zip", stage);
    snprintf(cmd, sizeof cmd, "%%SystemRoot%%\\System32\\curl.exe -L --fail -o \"%s\" %s", zip, PLATFORM_TOOLS);
    if (run(cmd)) fail("%s", "download failed (check the internet connection, or install platform-tools and add adb to PATH)");
    snprintf(cmd, sizeof cmd, "%%SystemRoot%%\\System32\\tar.exe -xf \"%s\" -C \"%s\"", zip, exe);
    if (run(cmd)) fail("%s", "could not unpack platform-tools");
    DeleteFileA(zip);
    if (!exists(adb)) fail("%s", "adb.exe missing after download");
}
static void adb_cmd(char *out, size_t n, const char *args) {
    char cmd[2048]; snprintf(cmd, sizeof cmd, "\"%s\" %s", adb, args);
    capture(cmd, out, n);
}
static void wait_for_phone(void) {
    char out[4096]; int said = -1;
    adb_cmd(out, sizeof out, "start-server 2>&1");
    for (int i = 0; i < 600; i++) {
        adb_cmd(out, sizeof out, "get-state 2>&1");
        int state = strstr(out, "unauthorized") ? 1 : strncmp(out, "device", 6) == 0 ? 2 : 0;
        if (state == 2) { printf("Phone connected.\n"); return; }
        if (state != said) {
            said = state;
            if (state == 1) printf("Allow USB debugging: tap \"Allow\" on the phone's prompt.\n");
            else printf("Connect the Android phone by USB and turn on USB debugging\n"
                        "(Settings > About phone > tap Build number 7 times, then Developer options > USB debugging).\n"
                        "Waiting for the phone...\n");
        }
        Sleep(1000);
    }
    fail("%s", "no phone found within 10 minutes");
}
static int pull(const char *remote, const char *local) {
    char cmd[2048]; snprintf(cmd, sizeof cmd, "\"%s\" pull \"%s\" \"%s\"", adb, remote, local);
    return run(cmd);
}
static void pull_from_phone(char *apk_dir, char *obb) {
    static char out[16384];
    find_adb();
    wait_for_phone();
    adb_cmd(out, sizeof out, "shell dumpsys package " PKG " | findstr versionName");
    if (out[0]) printf("Installed game: %s", out);
    adb_cmd(out, sizeof out, "shell pm path " PKG);
    if (!strstr(out, "package:")) fail("%s", "GTA: Liberty City Stories (" PKG ") is not installed on this phone");
    snprintf(apk_dir, MAX_PATH, "%s\\apk", stage); mkdirs(apk_dir);
    for (char *line = strtok(out, "\r\n"); line; line = strtok(0, "\r\n")) {
        char *path = strstr(line, "package:"); if (!path) continue;
        path += 8;
        const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
        char local[MAX_PATH]; snprintf(local, sizeof local, "%s\\%s", apk_dir, name);
        printf("Copying %s ...\n", name);
        if (pull(path, local)) fail("could not copy %s from the phone", name);
    }
    /* the radio needs the legacy expansion file, if the phone still has it */
    adb_cmd(out, sizeof out, "shell ls /sdcard/Android/obb/" PKG "/ 2>&1");
    for (char *line = strtok(out, "\r\n"); line; line = strtok(0, "\r\n")) {
        if (strncmp(line, "main.", 5) || !strstr(line, ".obb")) continue;
        char remote[512]; snprintf(remote, sizeof remote, "/sdcard/Android/obb/" PKG "/%s", line);
        snprintf(obb, MAX_PATH, "%s\\%s", stage, line);
        printf("Copying %s (radio) ...\n", line);
        if (pull(remote, obb)) obb[0] = 0;
        break;
    }
}

/* ---- APK unpacking ------------------------------------------------------------------------------------ */
static void unpack(const char *apk_dir) {
    char pattern[MAX_PATH], cmd[2048], out_dir[MAX_PATH]; WIN32_FIND_DATAA fd;
    int have_lib = 0;
    snprintf(out_dir, sizeof out_dir, "%s\\unpacked", stage); mkdirs(out_dir);
    snprintf(pattern, sizeof pattern, "%s\\*.apk", apk_dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) fail("no .apk files in %s", apk_dir);
    do {
        char apk[MAX_PATH]; snprintf(apk, sizeof apk, "%s\\%s", apk_dir, fd.cFileName);
        printf("Unpacking %s ...\n", fd.cFileName);
        /* members that are not in this split make tar report an error: ask for each part separately */
        snprintf(cmd, sizeof cmd, "%%SystemRoot%%\\System32\\tar.exe -xf \"%s\" -C \"%s\" assets 2>nul", apk, out_dir); run(cmd);
        snprintf(cmd, sizeof cmd, "%%SystemRoot%%\\System32\\tar.exe -xf \"%s\" -C \"%s\" lib/arm64-v8a/libGame.so lib/arm64-v8a/libopenal.so "
                 "lib/arm64-v8a/libVendor_mpg123.so 2>nul", apk, out_dir);
        if (run(cmd) == 0) have_lib = 1;
        /* launcher icon, used for lcs.exe's icon when the game is built on this PC */
        snprintf(cmd, sizeof cmd, "%%SystemRoot%%\\System32\\tar.exe -xf \"%s\" -C \"%s\" \"res/mipmap-*/ic_launcher.png\" 2>nul", apk, out_dir); run(cmd);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    char lib[MAX_PATH]; snprintf(lib, sizeof lib, "%s\\lib\\arm64-v8a\\libGame.so", out_dir);
    if (!have_lib || !exists(lib)) fail("%s", "the 64-bit ARM libraries (split_config.arm64_v8a.apk) are missing; "
                                             "the phone must run the 64-bit version of the game");
    char from[MAX_PATH], to[MAX_PATH];
    snprintf(from, sizeof from, "%s\\lib\\arm64-v8a", out_dir); snprintf(to, sizeof to, "%s\\lib", dest); merge_tree(from, to);
    snprintf(from, sizeof from, "%s\\assets", out_dir); snprintf(to, sizeof to, "%s\\assets", dest); merge_tree(from, to);
    snprintf(from, sizeof from, "%s\\res", out_dir);
    if (exists(from)) { snprintf(to, sizeof to, "%s\\res", dest); merge_tree(from, to); }
}

/* ---- radio WAD (DAWL archives, XOR 0xaf/0x66 by absolute byte parity) ----------------------------------- */
#pragma pack(push, 1)
typedef struct { uint32_t hash, a, b, c; uint64_t offset, size, size2, flags; } Entry;
#pragma pack(pop)
typedef struct { uint8_t header[2048]; uint8_t *dirs; uint32_t ndirs_bytes; Entry *e; uint32_t n; uint8_t *names; uint32_t names_size; } Wad;
static void crypt(uint8_t *d, size_t n, uint64_t off) { for (size_t i = 0; i < n; i++) d[i] ^= ((off + i) & 1) ? 0x66 : 0xaf; }
static int read_at(FILE *f, void *buf, size_t n, uint64_t off) {
    if (_fseeki64(f, (long long)off, SEEK_SET)) return 0;
    if (fread(buf, 1, n, f) != n) return 0;
    crypt(buf, n, off); return 1;
}
static int wad_load(const char *path, Wad *w) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint32_t count; uint64_t pos = 2048;
    memset(w, 0, sizeof *w);
    if (!read_at(f, w->header, 2048, 0) || memcmp(w->header, "DAWL\1\0\0\0\1\0\0\0", 12)) goto bad;
    if (!read_at(f, &count, 4, pos) || !count || count >= 100000) goto bad;
    pos += 4; w->ndirs_bytes = count * 8; w->dirs = malloc(w->ndirs_bytes);
    if (!read_at(f, w->dirs, w->ndirs_bytes, pos)) goto bad;
    pos += w->ndirs_bytes;
    if (!read_at(f, &w->n, 4, pos) || !w->n || w->n >= 100000) goto bad;
    pos += 4; w->e = malloc(w->n * sizeof(Entry));
    if (!read_at(f, w->e, w->n * sizeof(Entry), pos)) goto bad;
    pos += w->n * sizeof(Entry);
    if (!read_at(f, &w->names_size, 4, pos)) goto bad;
    w->names = malloc(w->names_size ? w->names_size : 1);
    if (!read_at(f, w->names, w->names_size, pos + 4)) goto bad;
    fclose(f); return 1;
bad: fclose(f); return 0;
}
static int is_mp3(FILE *f, const Entry *e) {
    uint8_t s[4] = {0};
    if (!read_at(f, s, e->size < 4 ? (size_t)e->size : 4, e->offset)) return 0;
    return !memcmp(s, "ID3", 3) || (s[0] == 0xff && (s[1] == 0xfb || s[1] == 0xf3 || s[1] == 0xf2));
}
static void build_radio(const char *obb, const char *base_wad, const char *out_path) {
    Wad src, base;
    if (!wad_load(obb, &src)) { printf("Radio: %s is not a supported OBB, skipped.\n", obb); return; }
    if (!wad_load(base_wad, &base)) { printf("Radio: cannot read %s, skipped.\n", base_wad); return; }
    FILE *in = fopen(obb, "rb");
    Entry *sel = malloc(src.n * sizeof(Entry)); uint32_t ns = 0;
    for (uint32_t i = 0; i < src.n; i++) {
        int known = 0;
        for (uint32_t j = 0; j < base.n && !known; j++) known = base.e[j].hash == src.e[i].hash;
        if (known || !is_mp3(in, &src.e[i]) || src.e[i].size != src.e[i].size2 || src.e[i].flags) continue;
        sel[ns++] = src.e[i];
    }
    if (!ns) { printf("Radio: the OBB has no missing radio tracks, skipped.\n"); fclose(in); return; }
    uint64_t meta = 2048 + 4 + src.ndirs_bytes + 4 + (uint64_t)ns * sizeof(Entry) + 4 + src.names_size;
    uint64_t start = (meta + 2047) & ~2047ull;
    char tmp[MAX_PATH]; snprintf(tmp, sizeof tmp, "%s.part", out_path);
    FILE *out = fopen(tmp, "wb+"); if (!out) fail("cannot write %s", tmp);
    uint8_t *buf = malloc(1 << 20);
    memset(buf, 0, 1 << 20);
    for (uint64_t left = start; left; ) { size_t k = left > (1 << 20) ? (1 << 20) : (size_t)left; fwrite(buf, 1, k, out); left -= k; }
    printf("Radio: copying %u tracks ...\n", ns);
    for (uint32_t i = 0; i < ns; i++) {
        uint64_t position = (uint64_t)_ftelli64(out), left = sel[i].size;
        int flip = ((position ^ sel[i].offset) & 1) != 0; /* payload stays encrypted: only the XOR phase changes */
        _fseeki64(in, (long long)sel[i].offset, SEEK_SET);
        while (left) {
            size_t k = left > (1 << 20) ? (1 << 20) : (size_t)left;
            if (fread(buf, 1, k, in) != k) fail("%s", "truncated OBB");
            if (flip) for (size_t j = 0; j < k; j++) buf[j] ^= 0xc9;
            fwrite(buf, 1, k, out); left -= k;
        }
        sel[i].offset = position;
    }
    uint8_t *m = calloc(1, start); uint64_t p = 0;
    memcpy(m, src.header, 2048); p = 2048;
    uint32_t nd = src.ndirs_bytes / 8; memcpy(m + p, &nd, 4); p += 4;
    memcpy(m + p, src.dirs, src.ndirs_bytes); p += src.ndirs_bytes;
    memcpy(m + p, &ns, 4); p += 4;
    memcpy(m + p, sel, ns * sizeof(Entry)); p += ns * sizeof(Entry);
    memcpy(m + p, &src.names_size, 4); p += 4;
    memcpy(m + p, src.names, src.names_size);
    crypt(m, start, 0);
    _fseeki64(out, 0, SEEK_SET); fwrite(m, 1, start, out);
    fclose(out); fclose(in);
    Wad check;
    if (!wad_load(tmp, &check) || check.n != ns) { DeleteFileA(tmp); fail("%s", "radio archive check failed"); }
    MoveFileExA(tmp, out_path, MOVEFILE_REPLACE_EXISTING);
    printf("Radio: %u tracks ready.\n", ns);
}

int main(int argc, char **argv) {
    char apk_dir[MAX_PATH] = "", obb[MAX_PATH] = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dest") && i + 1 < argc) snprintf(dest, sizeof dest, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--apks") && i + 1 < argc) snprintf(apk_dir, sizeof apk_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--obb") && i + 1 < argc) snprintf(obb, sizeof obb, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--adb") && i + 1 < argc) snprintf(adb, sizeof adb, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--no-pause")) pause_at_end = 0;
        else { printf("usage: lcs-data --dest DIR [--apks DIR] [--obb FILE] [--adb PATH] [--no-pause]\n"); return 2; }
    }
    if (!dest[0]) { printf("usage: lcs-data --dest DIR [--apks DIR] [--obb FILE]\n"); return 2; }
    printf("GTA: Liberty City Stories PC - game data setup\n"
           "The data is copied from your own copy of the Android game.\n\n");
    mkdirs(dest);
    snprintf(stage, sizeof stage, "%s\\.setup-temp", dest);
    remove_tree(stage); mkdirs(stage);
    if (!apk_dir[0]) pull_from_phone(apk_dir, obb);
    unpack(apk_dir);
    char music[MAX_PATH], base_wad[MAX_PATH];
    snprintf(music, sizeof music, "%s\\assets\\data_music.wad", dest);
    snprintf(base_wad, sizeof base_wad, "%s\\assets\\data_main.wad", dest);
    if (!exists(base_wad)) fail("%s", "data_main.wad is missing: the game's data pack was not installed on the phone "
                                     "(start the game once on the phone so it finishes downloading)");
    if (obb[0] && !exists(music)) build_radio(obb, base_wad, music);
    else if (!exists(music)) printf("Radio: no main.*.obb found; the game runs without radio stations.\n");
    remove_tree(stage);
    printf("\nDone. Game data is in %s\n", dest);
    finish(0);
    return 0;
}
