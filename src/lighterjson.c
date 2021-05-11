/**
 * @file      lighterjson.c
 * @brief     JSON minifier
 * @author    Aaron Kaluszka
 * @version   0.3.0
 * @date      1 Jan 2021
 * @copyright Copyright 2017-2021 Aaron Kaluszka
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

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct File {
  uint8_t* data_start;
  uint8_t* rindex;
  uint8_t* windex;
  uint8_t* lindex;
  uint8_t* data_end;
} File;

typedef struct Bitfield {
  size_t size;
  uint64_t* bits;
  size_t bit_level;
  size_t byte_level;
  size_t current;
} Bitfield;

typedef enum Container {None = -1, Array, Object} Container;

int64_t precision;
int quiet;
int newlines;

int do_file(char filename[]);

// Write queued data and move past data to skip
void write_data(File* file, ptrdiff_t index_offset) {
  memmove(file->windex, file->lindex, file->rindex - file->lindex);
  file->windex += file->rindex - file->lindex;
  file->rindex += index_offset;
  file->lindex = file->rindex;
}

void do_literal(File* file, const char* literal, size_t length) {
  if (strncmp((char*) file->rindex, literal, length)) {
    write_data(file, length);
  } else {
    file->rindex += length;
  }
}

uint64_t hex_value(File* file) {
  uint64_t value = 0;
  if (file->rindex + 4 > file->data_end) {
    return INT64_MAX;
  }
  for (size_t i = 0; i < 4; ++i) {
    const uint64_t x = file->rindex[i];
    const uint64_t shift = (3 - i) << 2;
    if (x >= '0' && x <= '9') {
      value += ((x - '0') << shift);
    } else if (x >= 'A' && x <= 'F') {
      value += ((x - 'A' + 10) << shift);
    } else if (x >= 'a' && x <= 'f') {
      value += ((x - 'a' + 10) << shift);
    } else {
      return INT64_MAX; // invalid hex
    }
  }
  return value;
}

void do_unicode(File* file) {
  uint64_t value = hex_value(file);
  uint64_t value2 = 0;
  if (value == INT64_MAX) {
    fprintf(stderr, "INVALID HEX\n");
    return;
  }
  if (value < 0x20) {
    *file->windex++ = '\\';
    switch (value) {
      case '\b':
        *file->windex++ = 'b';
        break;
      case '\f':
        *file->windex++ = 'f';
        break;
      case '\n':
        *file->windex++ = 'n';
        break;
      case '\r':
        *file->windex++ = 'r';
        break;
      case '\t':
        *file->windex++ = 't';
        break;
      default:
        file->lindex -= 1;
        file->rindex += 4;
        return;
    }
    write_data(file, 4);
    return;
  }
  write_data(file, 4);
  if (value < 0x80) {
    *file->windex++ = value;
    return;
  } else if (value < 0x800) {
    *file->windex++ = ((uint8_t)(value >> 6) & 0x1F) | 0xC0;
    *file->windex++ = ((uint8_t)(value & 0x3F)) | 0x80;
    return;
  }
  if (value & 0xD800) { // surrogate pair
    value2 = hex_value(file);
    if (value2 == INT64_MAX) {
      fprintf(stderr, "INVALID HEX\n");
      return;
    }
    if (value2 & 0xDC00) {
      value = (((value & 0x3FF) << 10) | (value2 & 0x3FF)) + 0x10000;
    }
    write_data(file, 4);
  }
  if (value < 0x10000) {
    *file->windex++ = ((uint8_t)(value >> 12) & 0xF) | 0xE0;
    *file->windex++ = (uint8_t)(value >> 6) & 0x3F;
    *file->windex++ = ((uint8_t)value & 0x3F) | 0x80;
  } else {
    *file->windex++ = ((uint8_t)(value >> 18) & 0x7) | 0xF0;
    *file->windex++ = ((uint8_t)(value >> 12) & 0x3F) | 0x80;
    *file->windex++ = ((uint8_t)(value >> 6) & 0x3F) | 0x80;
    *file->windex++ = ((uint8_t)value & 0x3F) | 0x80;
  }
}

void do_escape(File* file) {
  if (file->rindex + 1 < file->data_end) {
    switch (file->rindex[1]) {
      case 'u': // unicode
        write_data(file, 2);
        do_unicode(file);
        break;
      case '"':
      case '\\':
      case '/':
      case 'b':
      case 'f':
      case 'n':
      case 'r':
      case 't':
        file->rindex += 2;
        break;
      default:
        write_data(file, 1);
        ++(file->rindex);
    }
  } else {
    // TODO: error end of file
  }
}

void do_string(File* file) {
  ++(file->rindex);
  while (file->rindex < file->data_end) {
    switch (*file->rindex) {
      case '\\':
        do_escape(file);
        break;
      case '"':
        ++(file->rindex);
        return;
      default:
        ++(file->rindex);
    }
  }
}

int do_object_label(File* file) {
  while (file->rindex < file->data_end) {
    switch (*file->rindex) {
      case '"':
        do_string(file);
        return 0;
      case '}':
        return 1;
      default:
        write_data(file, 1);
    }
  }
  return 1;
}

void do_object(File* file) {
  if (do_object_label(file)) {
    return;
  }
  while (file->rindex < file->data_end) {
    switch (*file->rindex) {
      case ':':
        ++(file->rindex);
        return;
      default:
        write_data(file, 1);
    }
  }
}

void do_number(File* file) {
  uint8_t* decimal = 0;
  uint8_t* exponent = 0;
  uint8_t* non_zero_start = 0;
  uint8_t* non_zero_finish = 0;
  uint8_t* exponent_start = 0;
  uint8_t* number_end = 0;
  int64_t exponent_value = 0;
  int64_t min_exponent = 0;
  int64_t max_exponent = 0;
  int64_t new_decimal = 0;
  int64_t new_exponent = 0;
  uint64_t digit_width = 0;
  uint64_t new_exponent_width = 0;
  int negative = 0;
  int negative_exponent = 0;
  uint64_t zeroes = 0;
  uint8_t* i;
  uint64_t multiplier = 1;
  if (*file->rindex == '-') {
    negative = 1;
    ++(file->rindex);
  }
  for (i = file->rindex; i < file->data_end && !exponent && !number_end; ++i) {
    switch (*i) {
      case '.':
        decimal = i;
        break;      
      case 'e':
      case 'E':
        exponent = i;
        if (i + 1 < file->data_end) {
          switch (*(i + 1)) {
            case '-':
              negative_exponent = 1;
              // fallthrough
            case '+':
              ++i;
          }
        }
        break;
      case '0':
        break;
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        if (!non_zero_start) {
          non_zero_start = i;
        }
        non_zero_finish = i;
        break;
      default:
        number_end = i - 1;
    }
  }
  if (!number_end) {
    for (; i < file->data_end && !number_end; ++i) {
      switch (*i) {
        case '0':
          break;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
          if (!exponent_start) {
            exponent_start = i;
          }
          break;
        default:
          number_end = i - 1;
      }
    }
  }
  if (!number_end) {
    number_end = file->data_end - 1;
  }
  if (!non_zero_start) {
    if (negative) {
      --(file->rindex);
    }
    if (number_end == file->rindex) {
      ++(file->rindex);
    } else {
      write_data(file, number_end - file->rindex);
      *file->windex++ = '0';
    }
    return;
  }
  if (!exponent_start) {
    exponent_start = number_end;
  }
  if (exponent) {
    for (i = number_end; i >= exponent_start; --i) {
      exponent_value += (*i - '0') * multiplier;
      multiplier *= 10;
    }
  }
  if (negative_exponent) {
    exponent_value *= -1;
  }
  max_exponent = (int64_t)(decimal ? decimal > non_zero_start ? decimal - 1 : decimal : exponent ? exponent - 1 : number_end) - (int64_t)non_zero_start + exponent_value;
  min_exponent = (int64_t)(decimal ? decimal > non_zero_finish ? decimal - 1 : decimal : exponent ? exponent - 1 : number_end) - (int64_t)non_zero_finish + exponent_value;

  // Rounding
  if (-precision > max_exponent) {
    if (negative) {
      --(file->rindex);
    }
    write_data(file, number_end - file->rindex);
    *file->windex++ = '0';
    return;
  }
  if (-precision > min_exponent) {
    min_exponent = -precision;
    // find offset
    i = (decimal ? decimal > file->rindex + precision ? decimal - 1 : decimal : exponent ? exponent - 1 : number_end) + precision + exponent_value;
    if (i < non_zero_finish) {
      if ( *(i + 1) >= '5') {
        for (; i >= non_zero_start; --i) {
          if (*i == '9') {
            ++min_exponent;
          } else if (*i != '.') {
            ++*i;
            break;
          }
        }
        // if first position incremented
        if (i < non_zero_start) {
          *(++i) = '1';
          ++max_exponent;
        }
      }
      while (i >= non_zero_start && *i == '0') {
        --i;
        ++min_exponent;
      }
    }
    non_zero_finish = i;
  }

  digit_width = max_exponent - min_exponent + 1;
  if (min_exponent > 0) {
    zeroes = min_exponent;
  } else if (max_exponent < 0) {
    zeroes = -max_exponent;
  }
  if (zeroes < 3) {
    if (min_exponent < 0) {
      new_decimal = max_exponent >= 0 ? max_exponent + 1 : 1;
    }
  } else {
    new_exponent = min_exponent;
    zeroes = 0;
  }
  if (non_zero_start > file->rindex) {
    write_data(file, non_zero_start - file->rindex);
  }
  if (decimal == file->rindex + new_decimal && exponent_value == new_exponent) {
    file->rindex += zeroes + digit_width + 1;
  }
  else if (zeroes && max_exponent < 0) {
    i = file->windex;
    file->windex += zeroes + 1;
    if (non_zero_start < decimal && non_zero_finish > decimal) {
      write_data(file, decimal - non_zero_start + 1);
      file->rindex = non_zero_finish + 1;
      file->windex += decimal - non_zero_start;
      write_data(file, -digit_width - 1);
      file->rindex = decimal;
      file->windex = i + zeroes + 1;
      write_data(file, non_zero_finish - decimal);
      file->windex += non_zero_finish - decimal;
    } else {
      file->rindex = non_zero_finish + 1;
    }
    write_data(file, 0);
    *i++ = '0';
    *i++ = '.';
    if (zeroes > 1) {
      *i = '0';
    }
  } else {
    if (decimal) {
      if ((!new_decimal && non_zero_start < decimal && non_zero_finish > decimal) || (new_decimal && non_zero_start + new_decimal > decimal)) {
        file->rindex = decimal;
        write_data(file, 1);
      } else if (new_decimal && decimal && non_zero_start + new_decimal < decimal) {
        file->rindex = non_zero_start + new_decimal;
        write_data(file, 0);
        i = file->windex++;
        file->rindex = decimal;
        write_data(file, 1);
        *i = '.';
      }
    }
    if (new_decimal && (!decimal || non_zero_start + new_decimal > decimal)) {
      file->rindex = non_zero_start + new_decimal + (decimal ? 1 : 0);
      write_data(file, 0);
      i = file->windex++;
      file->rindex = non_zero_finish + 1;
      write_data(file, 0);
      *i = '.';
    } else {
      file->rindex = non_zero_finish + 1;
    }
    if (zeroes) {
      if (non_zero_finish + 1 + zeroes == (exponent ? exponent : number_end)) {
        file->rindex += zeroes;
      } else {
        write_data(file, 0);
        *file->windex++ = '0';
        if (zeroes > 1) {
          *file->windex++ = '0';
        }
      }
    }
  }
  if (exponent > file->rindex) {
    write_data(file, exponent - file->rindex);
  }
  if (new_exponent) {
    for (int64_t x = 1; new_exponent / x; x *= 10) {
      ++new_exponent_width;
    }
    if (exponent_value == new_exponent && number_end - exponent == new_exponent_width + negative_exponent) {
      file->rindex += new_exponent_width + negative_exponent + 1;
    } else {
      write_data(file, exponent_start ? exponent_start - file->rindex : 0);
      *file->windex++ = 'E';
      if (new_exponent < 0) {
        *file->windex++ = '-';
      }
      if (new_exponent == exponent_value) {
        file->rindex += new_exponent_width;
      } else {
        file->windex += new_exponent_width - 1;
        if (new_exponent < 0) {
          new_exponent = -new_exponent;
        }
        while (new_exponent) {
          *file->windex-- = new_exponent % 10 + '0';
          new_exponent /= 10;
        }
        file->windex += new_exponent_width + 1;
      }
    }
  }
  if (number_end > file->rindex) {
    write_data(file, number_end - file->rindex);
  }
}

void init_bits(Bitfield* bitfield) {
  bitfield->size = sizeof(uint64_t);
  bitfield->bits = (uint64_t*) malloc(bitfield->size);
  bitfield->bit_level = 0;
  bitfield->byte_level = 0;
  bitfield->current = -1;
}

void increment_bit(Bitfield* bitfield) {
  if (bitfield->bit_level == sizeof(uint64_t) * CHAR_BIT - 1) {
    bitfield->bit_level = 0;
    ++(bitfield->byte_level);
  } else {
    ++(bitfield->bit_level);
  }
  if (bitfield->byte_level > bitfield->size) {
    bitfield->bits = realloc(bitfield->bits, ++(bitfield->size));
  }
}

void push_set_bit(Bitfield* bitfield) {
  bitfield->current = 1;
  bitfield->bits[bitfield->byte_level] |= (1ULL << bitfield->bit_level);
  increment_bit(bitfield);
}

void push_clear_bit(Bitfield* bitfield) {
  bitfield->current = 0;
  bitfield->bits[bitfield->byte_level] &= ~(1ULL << bitfield->bit_level);
  increment_bit(bitfield);
}

void pop_bit(Bitfield* bitfield) {
  if (bitfield->bit_level == 0) {
    if (bitfield->byte_level > 0) {
      --(bitfield->byte_level);
      bitfield->bit_level = sizeof(uint64_t) * CHAR_BIT - 1;
    }
  } else {
    --(bitfield->bit_level);
  }
  bitfield->current = bitfield->bit_level > 0 || bitfield->byte_level > 0 ? (bitfield->bits[bitfield->byte_level] >> (bitfield->bit_level - 1)) & 1 : -1;
}

int do_value(File* file, int line_start) {
  Bitfield parent_types;
  init_bits(&parent_types);
  int comma_ok = 0;
  while (file->rindex < file->data_end) {
    switch (*file->rindex) {
      case '"':
        do_string(file);
        comma_ok = 1;
        break;
      case '{':
        ++(file->rindex);
        push_set_bit(&parent_types);
        do_object(file);
        comma_ok = 0;
        break;
      case '}':
        if (parent_types.current == Object) {
          ++(file->rindex);
          pop_bit(&parent_types);
          comma_ok = 1;
        } else {
          write_data(file, 1);
        }
        break;
      case '[':
        ++(file->rindex);
        push_clear_bit(&parent_types);
        comma_ok = 0;
        break;
      case ']':
        if (parent_types.current == Array) {
          ++(file->rindex);
          pop_bit(&parent_types);
          comma_ok = 1;
        } else {
          write_data(file, 1);
        }
        break;
      case ',':
        if (comma_ok && parent_types.current != None) {
          ++(file->rindex);
          if (parent_types.current == Object) {
            do_object(file);
          }
        } else {
          write_data(file, 1);
        }
        break;
      case 't':
        do_literal(file, "true", 4);
        comma_ok = 1;
        break;
      case 'f':
        do_literal(file, "false", 5);
        comma_ok = 1;
        break;
      case 'n':
        do_literal(file, "null", 4);
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
        do_number(file);
        comma_ok = 1;
        break;
      case '\n':
        switch (line_start) {
          case 1:
            --line_start;
            // fallthrough
          case 2:
            ++(file->rindex);
            break;
          default:
            write_data(file, 1);
        }
        break;
      default: // invalid or whitespace
        write_data(file, 1);
    }
  }
  free(parent_types.bits);
  return 0;
}

int do_dir(char path[]) {
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
      do_dir(entry->d_name);
      chdir("..");
    } else if (strstr(entry->d_name, ".json") - entry->d_name == strlen(entry->d_name) - 5) {
      do_file(entry->d_name);
    }
  }
  closedir(dir);
  return EXIT_SUCCESS;
}

int do_file(char filename[]) {
  File file = {0};
  int fd;
  struct stat sb = {0};
  int exit_code = EXIT_SUCCESS;
  fd = open(filename, O_RDWR); 
  if (fd < 0) {
    fprintf(stderr, "Could not open %s: %s\n", filename, strerror(errno));
    exit_code = EXIT_FAILURE;
    goto close_descriptors_and_return;
  }
  if (!quiet) {
    printf("%s: ", filename);
  }
  fstat(fd, &sb);
  file.data_start = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (file.data_start == MAP_FAILED) {
    fprintf(stderr, "Could not map file\n");
    exit_code = EXIT_FAILURE;
    goto close_descriptors_and_return;
  }
  file.rindex = file.windex = file.lindex = file.data_start;
  file.data_end = file.data_start + sb.st_size;
  if (file.data_end - file.data_start > 2 && (*file.data_start == 0 || *(file.data_start + 1) == 0)) {
    fprintf(stderr, "Only UTF-8 input is currently supported\n");
    exit_code = EXIT_FAILURE;
    goto close_descriptors_and_return;
  }
  do_value(&file, newlines == 2 ? 2 : 0); // clean up leading newlines in -n mode
  if (newlines) {
    while (file.rindex < file.data_end) {  
      do_value(&file, newlines);
    }
  }
  write_data(&file, 0);
  if (newlines == 1 && file.windex > file.data_start && *(file.windex - 1) == '\n') {
    --(file.windex); // clean up trailing newline in -n mode
  }
  if (msync(file.data_start, file.windex - file.data_start, MS_SYNC) < 0) {
    fprintf(stderr, "Could not sync file: %s\n", strerror(errno));
    exit_code = EXIT_FAILURE;
    goto close_descriptors_and_return;
  }
  if (!quiet) {
    printf("Saved %lu bytes\n", (unsigned long) (file.data_end - file.windex));
  }

  close_descriptors_and_return:
  if (file.data_start != 0 && file.data_start != MAP_FAILED) {
    munmap(file.data_start, sb.st_size);
  }
  if (fd >= 0) {
    // We truncate the file here because Cygwin mmap implementation opens a new file descriptor,
    // and ftruncate fails if that descriptor is open with a "Permission denied" error
    if (exit_code == EXIT_SUCCESS && ftruncate(fd, file.windex - file.data_start) < 0) {
      fprintf(stderr, "Could not truncate file to new size: %s. It may have garbage characters at the end\n", strerror(errno));
    }
    close(fd);
  }
  return exit_code;
}

void usage(char progname[], int status) {
  fprintf(EXIT_SUCCESS ? stdout : stderr,
          "Usage: %s [options] path\n"
          "JSON minifier\n"
          "Options:\n"
          "  -p N Numeric precision (number of decimal places; can be negative)\n"
          "  -n   Process NDJSON/JSON Lines\n"
          "  -N   Process NDJSON, preserving empty lines\n"
          "  -q   Suppress output\n", progname);
  exit(status);
}

int main(int argc, char* argv[]) {
  struct stat sb;
  int opt;
  int negative = 0;
  precision = INT64_MAX;
  quiet = 0;
  newlines = 0;
  char* i;
  while ((opt = getopt(argc, argv, "h?qnNp:")) != -1) {
    switch (opt) {
      case 'h':
      case '?':
        usage(argv[0], EXIT_SUCCESS);
        break;
      case 'q':
        quiet = 1;
        break;
      case 'n':
        newlines = 1;
        break;
      case 'N':
        newlines = 2;
        break;
      case 'p':
        if (!optarg) {
          usage(argv[0], EXIT_FAILURE);
        }
        i = optarg;
        if (*i == '-') {
          negative = 1;
          ++i;
        }
        precision = 0;
        for (; *i && precision < INT64_MAX; ++i) {
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
              if (precision > INT64_MAX / 10 || (precision == INT64_MAX / 10 && *i > '7')) {
                fprintf(stderr, "Precision limited to %lld\n", (long long int) INT64_MAX);
                precision = INT64_MAX;
              }
              precision = precision * 10 + *i - '0';
              break;
            default:
              fprintf(stderr, "Precision must be an integer\n");
              exit(EXIT_FAILURE);
          }
        }
        if (negative) {
          precision = -precision;
        }
        break;
      default:
        usage(argv[0], EXIT_FAILURE);
    }
  }
  if (argc - optind != 1) {
    usage(argv[0], EXIT_FAILURE);
  }
  stat(argv[optind], &sb);
  if (sb.st_mode & S_IFDIR) {
    return do_dir(argv[optind]);
  }
  return do_file(argv[optind]);
}
