/**
 * @file      lighterjson.c
 * @brief     JSON minifier
 * @author    Aaron Kaluszka
 * @version   1.0.0
 * @date      21 Feb 2026
 * @copyright Copyright 2017-2026 Aaron Kaluszka
 *            Licensed under the Apache License, Version 2.0 (the "License");
 *            you may not use this file except in compliance with the License.
 *            You may obtain a copy of the License at
 *                http://www.apache.org/licenses/LICENSE-2.0
 *            Unless required by applicable law or agreed to in writing, software
 *            distributed under the License is distributed on an "AS IS" BASIS,
 *            WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *            See the License for the specific language governing permissions and
 *            limitations under the License.
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64) || defined(WIN32)
#define LIGHTER_PLATFORM_WIN 1
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "lighter_bitfield.h"
#include "lighter_common.h"
#include "lighter_memmap.h"
#include "lighter_number.h"
#include "lighter_string.h"

typedef struct LighterContext {
  int64_t precision;
  int quiet;
  int newlines;
  int disable_nfc;
} LighterContext;

static int do_file(LighterContext* ctx, char filename[]);
static int do_dir(LighterContext* ctx, char path[]);

// Skip a run of whitespace; when include_newline is 0 (JSONL) don't include \n so case '\n' can handle it
void skip_whitespace_run(LighterData* data, int include_newline) {
  uint8_t* run = data->rindex;
  if (include_newline) {
    while (run < data->data_end && (*run == ' ' || *run == '\t' || *run == '\n' || *run == '\r')) ++run;
  } else {
    while (run < data->data_end && (*run == ' ' || *run == '\t' || *run == '\r')) ++run;
  }
  lighter_write_data(data, run - data->rindex);
}

void do_literal(LighterData* data, const char* literal, size_t length) {
  if (strncmp((char*) data->rindex, literal, length)) {
    lighter_write_data(data, length);
  } else {
    data->rindex += length;
  }
}

static void do_string(LighterData* data, LighterContext* ctx) {
  lighter_do_string(data, ctx->disable_nfc);
}

static int do_object_label(LighterData* data, LighterContext* ctx, int line_start) {
  while (data->rindex < data->data_end) {
    switch (*data->rindex) {
      case '"':
        do_string(data, ctx);
        return 0;
      case '}':
        return 1;
      case '\n':
        if (line_start) {
          /* NDJSON: newline inside object = end of line; stop without consuming */
          return 1;
        }
        skip_whitespace_run(data, 1);
        break;
      case ' ':
      case '\t':
      case '\r':
        skip_whitespace_run(data, 1);
        break;
      default:
        lighter_write_data(data, 1);
    }
  }
  return 1;
}

static void do_object(LighterData* data, LighterContext* ctx, int line_start) {
  if (do_object_label(data, ctx, line_start)) {
    return;
  }
  while (data->rindex < data->data_end) {
    switch (*data->rindex) {
      case ':':
        lighter_write_data(data, 0);   /* copy any pending (key or whitespace) */
        ++(data->rindex);
        lighter_write_data(data, 0);   /* copy colon so we never advance past what we copy */
        return;
      case '\n':
        if (line_start) {
          /* NDJSON: newline before colon = end of line; stop without consuming */
          return;
        }
        skip_whitespace_run(data, 1);
        break;
      case ' ':
      case '\t':
      case '\r':
        skip_whitespace_run(data, 1);
        break;
      default:
        lighter_write_data(data, 1);
    }
  }
}

static void do_number(LighterData* data, LighterContext* ctx) {
  lighter_do_number(data, ctx->precision);
}

static int do_value(LighterData* data, LighterContext* ctx, int line_start) {
  Bitfield parent_types;
  init_bits(&parent_types);
  int comma_ok = 0;
  while (data->rindex < data->data_end) {
    switch (*data->rindex) {
      case '"':
        do_string(data, ctx);
        comma_ok = 1;
        break;
      case '{':
        ++(data->rindex);
        push_set_bit(&parent_types);
        do_object(data, ctx, line_start);
        comma_ok = 0;
        break;
      case '}':
        if (parent_types.current == Object) {
          ++(data->rindex);
          pop_bit(&parent_types);
          comma_ok = 1;
        } else {
          lighter_write_data(data, 1);
        }
        break;
      case '[':
        ++(data->rindex);
        push_clear_bit(&parent_types);
        comma_ok = 0;
        break;
      case ']':
        if (parent_types.current == Array) {
          ++(data->rindex);
          pop_bit(&parent_types);
          comma_ok = 1;
        } else {
          lighter_write_data(data, 1);
        }
        break;
      case ',':
        if (comma_ok && parent_types.current != None) {
          ++(data->rindex);
          if (parent_types.current == Object) {
            do_object(data, ctx, line_start);
          }
        } else {
          lighter_write_data(data, 1);
        }
        break;
      case 't':
        do_literal(data, "true", 4);
        comma_ok = 1;
        break;
      case 'f':
        do_literal(data, "false", 5);
        comma_ok = 1;
        break;
      case 'n':
        do_literal(data, "null", 4);
        comma_ok = 1;
        break;
      case '-':
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        do_number(data, ctx);
        comma_ok = 1;
        break;
      case '\n':
        switch (line_start) {
          case 1:
            --line_start;
            // fallthrough
          case 2:
            ++(data->rindex);
            break;
          default:
            skip_whitespace_run(data, 1);
        }
        break;
      case ' ':
      case '\t':
      case '\r':
        skip_whitespace_run(data, line_start ? 0 : 1);
        break;
      default: // invalid
        lighter_write_data(data, 1);
    }
  }
  free(parent_types.bits);
  return 0;
}

/* Return 1 if filename should be processed: .json always; .jsonl/.ndjson when in NDJSON mode. */
static int dir_should_process(LighterContext* ctx, const char* name) {
  size_t len = strlen(name);
  return (len >= 5 && strcmp(name + len - 5, ".json") == 0) ||
         (ctx->newlines && ((len >= 6 && strcmp(name + len - 6, ".jsonl") == 0) || 
                            (len >= 7 && strcmp(name + len - 7, ".ndjson") == 0)));
}

#if LIGHTER_PLATFORM_WIN
static int do_dir_win(LighterContext* ctx, const char* path) {
  size_t plen = strlen(path);
  wchar_t* long_wpath = lighter_make_long_path_w(path);
  if (!long_wpath) {
    return EXIT_FAILURE;
  }

  size_t wplen = wcslen(long_wpath);
  wchar_t* search_path = (wchar_t*)malloc((wplen + 3) * sizeof(wchar_t));
  if (!search_path) {
    free(long_wpath);
    return EXIT_FAILURE;
  }
  wcscpy(search_path, long_wpath);

  if (wplen > 0 && search_path[wplen - 1] != L'\\' && search_path[wplen - 1] != L'/') {
    search_path[wplen] = L'\\';
    search_path[wplen + 1] = L'*';
    search_path[wplen + 2] = L'\0';
  } else {
    search_path[wplen] = L'*';
    search_path[wplen + 1] = L'\0';
  }
  free(long_wpath);

  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(search_path, &fd);
  free(search_path);

  if (h == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "Could not open %s\n", path);
    return EXIT_FAILURE;
  }
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
      continue;
    }

    int ulen = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, NULL, 0, NULL, NULL);
    if (ulen <= 0) {
      continue;
    }
    char* utf8name = (char* )malloc(ulen);
    if (!utf8name) {
      continue;
    }
    WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, utf8name, ulen, NULL,
                        NULL);

    size_t sub_size = plen + strlen(utf8name) + 3;
    char* sub = (char* )malloc(sub_size);
    if (!sub) {
      free(utf8name);
      continue;
    }

    snprintf(sub, sub_size, "%s%s%s", path,
             (plen > 0 && path[plen - 1] != '\\' && path[plen - 1] != '/')
                 ? "\\"
                 : "",
             utf8name);

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      do_dir_win(ctx, sub);
    } else if (dir_should_process(ctx, utf8name)) {
      do_file(ctx, sub);
    }
    free(sub);
    free(utf8name);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return EXIT_SUCCESS;
}
static int do_dir(LighterContext* ctx, char path[]) {
  return do_dir_win(ctx, path);
}
#else
static int do_dir(LighterContext* ctx, char path[]) {
  DIR *dir;
  struct dirent *entry;
  dir = opendir(path);
  if (!dir) {
    fprintf(stderr, "Could not open %s: %s\n", path, strerror(errno));
    return EXIT_FAILURE;
  }
  chdir(path);
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (entry->d_type == DT_DIR) {
      do_dir(ctx, entry->d_name);
      chdir("..");
    } else if (dir_should_process(ctx, entry->d_name)) {
      do_file(ctx, entry->d_name);
    }
  }
  closedir(dir);
  return EXIT_SUCCESS;
}
#endif

static int do_file(LighterContext* ctx, char filename[]) {
  LighterMap map = {0};
  if (lighter_map_open(&map, filename, 0) != 0) {
    return EXIT_FAILURE;
  }
  if (!ctx->quiet) {
    printf("%s: ", filename);
  }
  LighterData data;
  data.data_start = data.rindex = data.windex = data.lindex = map.data;
  data.data_end = map.data + map.size;

  int exit_code = EXIT_SUCCESS;
  if (data.data_end - data.data_start > 2 && (*data.data_start == 0 || *(data.data_start + 1) == 0)) {
    fprintf(stderr, "Only UTF-8 input is currently supported\n");
    exit_code = EXIT_FAILURE;
    goto cleanup;
  }
  do_value(&data, ctx, ctx->newlines == 2 ? 2 : 0); /* clean up leading newlines in -N mode */
  if (ctx->newlines) {
    while (data.rindex < data.data_end) {
      do_value(&data, ctx, ctx->newlines);
    }
  }
  lighter_write_data(&data, 0);
  if (ctx->newlines == 1 && data.windex > data.data_start && *(data.windex - 1) == '\n') {
    --(data.windex); /* clean up trailing newline in -n mode */
  }

  size_t written = (size_t)(data.windex - data.data_start);
  if (written > 0 && lighter_map_sync(&map, written) != 0) {
    fprintf(stderr, "Could not sync file\n");
    exit_code = EXIT_FAILURE;
  }
  if (exit_code == EXIT_SUCCESS && written > 0 && lighter_map_truncate(&map, written) != 0) {
    fprintf(stderr, "Could not truncate file. It may have garbage at the end\n");
  }

cleanup:
  lighter_map_close(&map);
  if (!ctx->quiet && data.data_end > data.data_start) {
    printf("Saved %lu bytes\n", (unsigned long)(data.data_end - data.windex));
  }
  return exit_code;
}

void usage(char progname[], int status) {
  fprintf(
      status == EXIT_SUCCESS ? stdout : stderr,
      "Usage: %s [options] path\n"
      "JSON minifier\n"
      "Options:\n"
      "  -p N Numeric precision (number of decimal places; can be negative)\n"
      "  -n   Process NDJSON/JSON Lines\n"
      "  -N   Process NDJSON, preserving empty lines\n"
      "  -U   Disable Unicode normalization\n"
      "  -q   Suppress output\n",
      progname);
  exit(status);
}

int main(int argc, char* argv[]) {
  int negative = 0;
  LighterContext ctx = {
      .precision = LIGHTER_PRECISION_UNLIMITED,
      .quiet = 0,
      .newlines = 0,
      .disable_nfc = 0,
  };
  char* i;
  int optind_val = 1;

  while (optind_val < argc && argv[optind_val][0] == '-' && argv[optind_val][1]) {
    char* o = argv[optind_val] + 1;
    int skip_rest = 0;
    if (*o == '-') {
      ++o;
      if (!*o) {
        break;
      }
    }
    for (; *o; ++o) {
      switch (*o) {
        case 'h':
        case '?':
          usage(argv[0], EXIT_SUCCESS);
          break;
        case 'q':
          ctx.quiet = 1;
          break;
        case 'n':
          ctx.newlines = 1;
          break;
        case 'N':
          ctx.newlines = 2;
          break;
        case 'U':
          ctx.disable_nfc = 1;
          break;
        case 'p': {
          if (o[1]) {
            i = o + 1;
          } else if (optind_val + 1 < argc) {
            i = argv[optind_val + 1];
            ++optind_val; /* consume next arg */
          } else {
            usage(argv[0], EXIT_FAILURE);
          }
          negative = 0;
          if (*i == '-') {
            negative = 1;
            ++i;
          }
          ctx.precision = 0;
          for (; *i && ctx.precision < LIGHTER_PRECISION_UNLIMITED; ++i) {
            switch (*i) {
              case '0':
              case '1':
              case '2':
              case '3':
              case '4':
              case '5':
              case '6':
              case '7':
              case '8':
              case '9':
                if (ctx.precision > LIGHTER_PRECISION_UNLIMITED / 10 || (ctx.precision == LIGHTER_PRECISION_UNLIMITED / 10 && *i > '7')) {
                  fprintf(stderr, "Precision limited to %lld\n", (long long) LIGHTER_PRECISION_UNLIMITED);
                  ctx.precision = LIGHTER_PRECISION_UNLIMITED;
                }
                ctx.precision = ctx.precision * 10 + *i - '0';
                break;
              default:
                fprintf(stderr, "Precision must be an integer\n");
                exit(EXIT_FAILURE);
            }
          }
          if (negative) {
            ctx.precision = -ctx.precision;
          }
          skip_rest = 1;
          break;
        }
        default:
          usage(argv[0], EXIT_FAILURE);
      }
      if (skip_rest) {
        break;
      }
    }
    ++optind_val;
  }

  if (argc - optind_val != 1) {
    usage(argv[0], EXIT_FAILURE);
  }

#if LIGHTER_PLATFORM_WIN
  DWORD att = INVALID_FILE_ATTRIBUTES;
  wchar_t* wpath = lighter_make_long_path_w(argv[optind_val]);
  if (wpath) {
    att = GetFileAttributesW(wpath);
    free(wpath);
  }
  if (att != INVALID_FILE_ATTRIBUTES && (att & FILE_ATTRIBUTE_DIRECTORY)) {
    return do_dir(&ctx, argv[optind_val]);
  }
#else
  struct stat sb;
  if (stat(argv[optind_val], &sb) == 0 && (sb.st_mode & S_IFDIR)) {
    return do_dir(&ctx, argv[optind_val]);
  }
#endif
  return do_file(&ctx, argv[optind_val]);
}
