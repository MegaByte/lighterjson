/**
 * @file   lighter_memmap.h
 * @brief  Platform-agnostic read-write file mapping (mmap / CreateFileMapping).
 */

#ifndef LIGHTER_MEMMAP_H
#define LIGHTER_MEMMAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64) || defined(WIN32)
  #include <windows.h>
  #define LIGHTER_MEMMAP_WIN 1
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <string.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#if LIGHTER_MEMMAP_WIN
static inline wchar_t* lighter_make_long_path_w(const char* utf8_path) {
  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, NULL, 0);
  if (wlen <= 0) {
    return NULL;
  }
  wchar_t* wpath = (wchar_t*)malloc(wlen * sizeof(wchar_t));
  if (!wpath) {
    return NULL;
  }
  MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wpath, wlen);

  DWORD full_len = GetFullPathNameW(wpath, 0, NULL, NULL);
  if (full_len == 0) {
    free(wpath);
    return NULL;
  }
  wchar_t* full_wpath = (wchar_t*)malloc(full_len * sizeof(wchar_t));
  if (!full_wpath) {
    free(wpath);
    return NULL;
  }
  GetFullPathNameW(wpath, full_len, full_wpath, NULL);
  free(wpath);

  if (wcsncmp(full_wpath, L"\\\\?\\", 4) == 0 || wcsncmp(full_wpath, L"\\\\.\\", 4) == 0) {
    return full_wpath;
  }

  size_t prefix_len = 4;
  int is_unc = (full_wpath[0] == L'\\' && full_wpath[1] == L'\\');
  if (is_unc) {
    prefix_len = 8;
  }

  wchar_t* long_wpath = (wchar_t*)malloc((full_len + prefix_len) * sizeof(wchar_t));
  if (!long_wpath) {
    free(full_wpath);
    return NULL;
  }

  if (is_unc) {
    wcscpy(long_wpath, L"\\\\?\\UNC\\");
    wcscat(long_wpath, full_wpath + 2);
  } else {
    wcscpy(long_wpath, L"\\\\?\\");
    wcscat(long_wpath, full_wpath);
  }
  free(full_wpath);
  return long_wpath;
}
#endif

typedef struct LighterMap {
  uint8_t* data;
  size_t size;
#if LIGHTER_MEMMAP_WIN
  HANDLE hFile;
  HANDLE hMap;
#else
  int fd;
#endif
} LighterMap;

/**
 * Map a file. read_only: 1 = read-only, 0 = read-write.
 * On success sets m->data and m->size and returns 0.
 * On failure returns -1 and reports to stderr; caller must not call lighter_map_close.
 */
static inline int lighter_map_open(LighterMap* m, const char* path, int read_only) {
#if LIGHTER_MEMMAP_WIN
  wchar_t* wpath = lighter_make_long_path_w(path);
  if (!wpath) {
    fprintf(stderr, "Invalid path encoding or out of memory\n");
    return -1;
  }

  HANDLE h = CreateFileW(wpath, read_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE), FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  free(wpath);
  if (h == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "Could not open %s\n", path);
    return -1;
  }
  LARGE_INTEGER li;
  if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || (read_only && li.QuadPart > (LONGLONG)((size_t)-1))) {
    if (!read_only) {
      fprintf(stderr, "Could not get file size\n");
    }
    CloseHandle(h);
    return -1;
  }
  m->size = (size_t)li.QuadPart;
  m->hMap = CreateFileMappingA(h, NULL, read_only ? PAGE_READONLY : PAGE_READWRITE, 0, 0, NULL);
  if (!m->hMap) {
    if (!read_only) {
      fprintf(stderr, "Could not map file\n");
    }
    CloseHandle(h);
    return -1;
  }
  m->data = (uint8_t*)MapViewOfFile(m->hMap, read_only ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS, 0, 0, read_only ? m->size : 0);
  if (!m->data) {
    if (!read_only) {
      fprintf(stderr, "Could not map view\n");
    }
    CloseHandle(m->hMap);
    CloseHandle(h);
    return -1;
  }
  m->hFile = read_only ? INVALID_HANDLE_VALUE : h;
  if (read_only) {
    CloseHandle(h);
  }
  return 0;
#else
  m->fd = open(path, read_only ? O_RDONLY : O_RDWR);
  if (m->fd < 0) {
    fprintf(stderr, "Could not open %s: %s\n", path, strerror(errno));
    return -1;
  }
  struct stat sb;
  if (fstat(m->fd, &sb) < 0 || sb.st_size <= 0) {
    fprintf(stderr, "Could not get file size\n");
    close(m->fd);
    m->fd = -1;
    return -1;
  }
  m->size = (size_t)sb.st_size;
  m->data = (uint8_t*)mmap(NULL, m->size, read_only ? PROT_READ : (PROT_READ | PROT_WRITE), read_only ? MAP_PRIVATE : MAP_SHARED, m->fd, 0);
  if (m->data == MAP_FAILED) {
    fprintf(stderr, "Could not map file\n");
    close(m->fd);
    m->fd = -1;
    m->data = NULL;
    return -1;
  }
  return 0;
#endif
}

/**
 * Flush the first len bytes of the mapping to disk.
 */
static inline int lighter_map_sync(LighterMap* m, size_t len, int async_io) {
#if LIGHTER_MEMMAP_WIN
  (void)async_io;
  return FlushViewOfFile(m->data, len) ? 0 : -1;
#else
  return msync(m->data, len, async_io ? MS_ASYNC : MS_SYNC);
#endif
}

/**
 * Truncate the file to len bytes. Call after unmapping or with len already synced.
 */
static inline int lighter_map_truncate(LighterMap* m, size_t len) {
#if LIGHTER_MEMMAP_WIN
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)len;
  return (SetFilePointerEx(m->hFile, li, NULL, FILE_BEGIN) && SetEndOfFile(m->hFile)) ? 0 : -1;
#else
  return ftruncate(m->fd, (off_t)len);
#endif
}

/**
 * Unmap and close. Only call after a successful lighter_map_open.
 */
static inline void lighter_map_close(LighterMap* m) {
  if (!m->data) {
    return;
  }
#if LIGHTER_MEMMAP_WIN
  UnmapViewOfFile(m->data);
  m->data = NULL;
  if (m->hMap) {
    CloseHandle(m->hMap);
    m->hMap = NULL;
  }
  if (m->hFile != INVALID_HANDLE_VALUE) {
    CloseHandle(m->hFile);
    m->hFile = INVALID_HANDLE_VALUE;
  }
#else
  munmap(m->data, m->size);
  m->data = NULL;
  if (m->fd >= 0) {
    close(m->fd);
    m->fd = -1;
  }
#endif
}

#endif /* LIGHTER_MEMMAP_H */
