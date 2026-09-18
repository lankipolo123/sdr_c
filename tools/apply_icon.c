/* Patches a target .exe's actual file icon (the one Windows Explorer
 * shows, baked into the PE resource section at compile time - NOT
 * something a running app can change about itself, see
 * branding/README.md) by replacing its RT_GROUP_ICON/RT_ICON resources
 * with the contents of a .ico file, using the same UpdateResource API
 * a resource compiler itself uses. Meant to run as a post-build/post-
 * install step: build the app normally, then run this against the
 * built exe with branding\icon.ico to rebrand its file icon.
 *
 * Build (same mingw setup as the app itself, see build.bat):
 *   x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -Wpedantic -Werror -O2 -o apply_icon.exe apply_icon.c
 *
 * Usage: apply_icon.exe <target.exe> <icon.ico>
 *
 * Targets RT_GROUP_ICON id 100 specifically - matches IDI_APP_ICON in
 * resource.h/app.rc, the app's one and only icon group. The individual
 * images get fresh RT_ICON ids (101, 102, ...) rather than trying to
 * reuse whatever windres originally assigned - internally consistent
 * either way since the group resource we write encodes exactly which
 * ids to look up; any old RT_ICON entries this doesn't touch just
 * become harmless unreferenced data.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#pragma pack(push, 1)
typedef struct {
    WORD reserved;
    WORD type;
    WORD count;
} IcoDirHeader;

typedef struct {
    BYTE width;
    BYTE height;
    BYTE colorCount;
    BYTE reserved;
    WORD planes;
    WORD bitCount;
    DWORD bytesInRes;
    DWORD imageOffset;
} IcoDirEntry;

typedef struct {
    BYTE width;
    BYTE height;
    BYTE colorCount;
    BYTE reserved;
    WORD planes;
    WORD bitCount;
    DWORD bytesInRes;
    WORD id;
} GrpIconDirEntry;
#pragma pack(pop)

#define APP_ICON_GROUP_ID 100
#define FIRST_ICON_IMAGE_ID 101

static BOOL CALLBACK lang_cb(HMODULE hModule, LPCSTR type, LPCSTR name, WORD lang, LONG_PTR out) {
    (void)hModule; (void)type; (void)name;
    *(WORD *)out = lang;
    return FALSE; /* stop after the first one - this app only ever ships one */
}

/* Win32 keys a resource by (type, name, language) together, not just
 * (type, name) - UpdateResourceA at the WRONG language doesn't replace
 * the existing icon, it ADDS a second one alongside it (confirmed with
 * wrestool: windres/RC built this app's icon at language 1033, en-US,
 * not the neutral 0 UpdateResourceA defaults to without this). Explorer
 * then has two RT_GROUP_ICON(100) entries to pick from, an ambiguous,
 * easy-to-get-wrong state - so this reads the ACTUAL language already
 * on the target exe's icon group first and writes the replacement at
 * that exact language instead of guessing neutral. */
static WORD detect_icon_group_language(const char *exe_path) {
    HMODULE hModule;
    WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    hModule = LoadLibraryExA(exe_path, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (hModule) {
        EnumResourceLanguagesA(hModule, RT_GROUP_ICON, MAKEINTRESOURCEA(APP_ICON_GROUP_ID),
                                lang_cb, (LONG_PTR)&lang);
        FreeLibrary(hModule);
    }
    return lang;
}

int main(int argc, char **argv) {
    const char *exe_path, *ico_path;
    FILE *f;
    long size;
    unsigned char *ico_buf;
    IcoDirHeader *dir;
    IcoDirEntry *entries;
    int count, i;
    HANDLE hUpdate;
    unsigned char *grp_buf;
    size_t grp_size;
    GrpIconDirEntry *grp_entries;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <target.exe> <icon.ico>\n", argv[0]);
        return 1;
    }
    exe_path = argv[1];
    ico_path = argv[2];

    f = fopen(ico_path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", ico_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < (long)sizeof(IcoDirHeader)) {
        fprintf(stderr, "%s is too small to be a valid .ico\n", ico_path);
        fclose(f);
        return 1;
    }
    ico_buf = (unsigned char *)malloc((size_t)size);
    if (!ico_buf || fread(ico_buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "Failed to read %s\n", ico_path);
        fclose(f);
        return 1;
    }
    fclose(f);

    dir = (IcoDirHeader *)ico_buf;
    if (dir->reserved != 0 || dir->type != 1 || dir->count == 0) {
        fprintf(stderr, "%s is not a valid .ico file\n", ico_path);
        free(ico_buf);
        return 1;
    }
    count = dir->count;
    entries = (IcoDirEntry *)(ico_buf + sizeof(IcoDirHeader));

    for (i = 0; i < count; i++) {
        if ((long)(entries[i].imageOffset + entries[i].bytesInRes) > size) {
            fprintf(stderr, "%s: image %d's data runs past end of file\n", ico_path, i);
            free(ico_buf);
            return 1;
        }
    }

    {
        WORD icon_lang = detect_icon_group_language(exe_path);

        hUpdate = BeginUpdateResourceA(exe_path, FALSE);
        if (!hUpdate) {
            fprintf(stderr, "BeginUpdateResource(%s) failed: %lu\n", exe_path, GetLastError());
            free(ico_buf);
            return 1;
        }

        for (i = 0; i < count; i++) {
            WORD id = (WORD)(FIRST_ICON_IMAGE_ID + i);
            if (!UpdateResourceA(hUpdate, RT_ICON, MAKEINTRESOURCEA(id), icon_lang,
                                  ico_buf + entries[i].imageOffset, entries[i].bytesInRes)) {
                fprintf(stderr, "UpdateResource(RT_ICON, %d) failed: %lu\n", id, GetLastError());
                EndUpdateResourceA(hUpdate, TRUE); /* discard partial changes */
                free(ico_buf);
                return 1;
            }
        }

        grp_size = sizeof(IcoDirHeader) + (size_t)count * sizeof(GrpIconDirEntry);
        grp_buf = (unsigned char *)malloc(grp_size);
        if (!grp_buf) {
            fprintf(stderr, "Out of memory\n");
            EndUpdateResourceA(hUpdate, TRUE);
            free(ico_buf);
            return 1;
        }
        memcpy(grp_buf, dir, sizeof(IcoDirHeader));
        grp_entries = (GrpIconDirEntry *)(grp_buf + sizeof(IcoDirHeader));
        for (i = 0; i < count; i++) {
            grp_entries[i].width = entries[i].width;
            grp_entries[i].height = entries[i].height;
            grp_entries[i].colorCount = entries[i].colorCount;
            grp_entries[i].reserved = entries[i].reserved;
            grp_entries[i].planes = entries[i].planes;
            grp_entries[i].bitCount = entries[i].bitCount;
            grp_entries[i].bytesInRes = entries[i].bytesInRes;
            grp_entries[i].id = (WORD)(FIRST_ICON_IMAGE_ID + i);
        }

        if (!UpdateResourceA(hUpdate, RT_GROUP_ICON, MAKEINTRESOURCEA(APP_ICON_GROUP_ID),
                              icon_lang, grp_buf, (DWORD)grp_size)) {
            fprintf(stderr, "UpdateResource(RT_GROUP_ICON) failed: %lu\n", GetLastError());
            EndUpdateResourceA(hUpdate, TRUE);
            free(ico_buf);
            free(grp_buf);
            return 1;
        }

        if (!EndUpdateResourceA(hUpdate, FALSE)) {
            fprintf(stderr, "EndUpdateResource failed: %lu\n", GetLastError());
            free(ico_buf);
            free(grp_buf);
            return 1;
        }

        free(ico_buf);
        free(grp_buf);
    }
    printf("Applied %s's icon (%d image%s) to %s\n", ico_path, count, count == 1 ? "" : "s", exe_path);
    return 0;
}
