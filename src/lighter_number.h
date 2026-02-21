/**
 * @file   lighter_number.h
 * @brief  JSON number parsing and reformatting (self-contained header).
 */

#ifndef LIGHTER_NUMBER_H
#define LIGHTER_NUMBER_H

#include <stdint.h>

#include "lighter_common.h"

/** Parse and optionally reformat a JSON number. precision: LIGHTER_PRECISION_UNLIMITED = preserve form. */
static inline void lighter_do_number(LighterData* data, int64_t precision) {
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
  if (*data->rindex == '-') {
    negative = 1;
    ++(data->rindex);
  }
  for (i = data->rindex; i < data->data_end && !exponent && !number_end; ++i) {
    switch (*i) {
      case '.':
        decimal = i;
        break;
      case 'e':
      case 'E':
        exponent = i;
        if (i + 1 < data->data_end) {
          switch (*(i + 1)) {
            case '-':
              negative_exponent = 1;
              /* fallthrough */
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
    for (; i < data->data_end && !number_end; ++i) {
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
    number_end = data->data_end - 1;
  }
  if (!non_zero_start) {
    if (negative) {
      --(data->rindex);
    }
    if (number_end == data->rindex) {
      ++(data->rindex);
    } else {
      lighter_write_data(data, number_end - data->rindex);
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

  if (-precision > max_exponent) {
    if (negative) {
      --(data->rindex);
    }
    lighter_write_data(data, number_end - data->rindex);
    *data->windex++ = '0';
    return;
  }
  if (-precision > min_exponent) {
    min_exponent = -precision;
    i = (decimal ? decimal > data->rindex + precision ? decimal - 1 : decimal : exponent ? exponent - 1 : number_end) + precision + exponent_value;
    if (i < non_zero_finish) {
      if (*(i + 1) >= '5') {
        for (; i >= non_zero_start; --i) {
          if (*i == '9') {
            ++min_exponent;
          } else if (*i != '.') {
            ++*i;
            break;
          }
        }
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
  if (non_zero_start > data->rindex) {
    lighter_write_data(data, non_zero_start - data->rindex);
  }
  if (decimal == data->rindex + new_decimal && exponent_value == new_exponent) {
    data->rindex += zeroes + digit_width + 1;
  } else if (zeroes && max_exponent < 0) {
    i = data->windex;
    data->windex += zeroes + 1;
    if (non_zero_start < decimal && non_zero_finish > decimal) {
      lighter_write_data(data, decimal - non_zero_start + 1);
      data->rindex = non_zero_finish + 1;
      data->windex += decimal - non_zero_start;
      lighter_write_data(data, -digit_width - 1);
      data->rindex = decimal;
      data->windex = i + zeroes + 1;
      lighter_write_data(data, non_zero_finish - decimal);
      data->windex += non_zero_finish - decimal;
    } else {
      data->rindex = non_zero_finish + 1;
    }
    lighter_write_data(data, 0);
    *i++ = '0';
    *i++ = '.';
    if (zeroes > 1) {
      *i = '0';
    }
  } else {
    if (decimal) {
      if ((!new_decimal && non_zero_start < decimal && non_zero_finish > decimal) || (new_decimal && non_zero_start + new_decimal > decimal)) {
        data->rindex = decimal;
        lighter_write_data(data, 1);
      } else if (new_decimal && decimal && non_zero_start + new_decimal < decimal) {
        data->rindex = non_zero_start + new_decimal;
        lighter_write_data(data, 0);
        i = data->windex++;
        data->rindex = decimal;
        lighter_write_data(data, 1);
        *i = '.';
      }
    }
    if (new_decimal && (!decimal || non_zero_start + new_decimal > decimal)) {
      data->rindex = non_zero_start + new_decimal + (decimal ? 1 : 0);
      lighter_write_data(data, 0);
      i = data->windex++;
      data->rindex = non_zero_finish + 1;
      lighter_write_data(data, 0);
      *i = '.';
      if (!decimal) {
        *data->windex++ = '0';
      }
    } else {
      data->rindex = non_zero_finish + 1;
    }
    if (zeroes) {
      if (non_zero_finish + 1 + zeroes == (exponent ? exponent : number_end)) {
        data->rindex += zeroes;
      } else {
        lighter_write_data(data, 0);
        *data->windex++ = '0';
        if (zeroes > 1) {
          *data->windex++ = '0';
        }
      }
    }
  }
  if (exponent > data->rindex) {
    lighter_write_data(data, exponent - data->rindex);
  }
  if (new_exponent) {
    for (int64_t x = 1; new_exponent / x; x *= 10) {
      ++new_exponent_width;
    }
    if (exponent_value == new_exponent && number_end - exponent == new_exponent_width + negative_exponent) {
      data->rindex += new_exponent_width + negative_exponent + 1;
    } else {
      lighter_write_data(data, exponent_start ? exponent_start - data->rindex : 0);
      *data->windex++ = 'E';
      if (new_exponent < 0) {
        *data->windex++ = '-';
      }
      if (new_exponent == exponent_value) {
        data->rindex += new_exponent_width;
      } else {
        data->windex += new_exponent_width - 1;
        if (new_exponent < 0) {
          new_exponent = -new_exponent;
        }
        while (new_exponent) {
          *data->windex-- = new_exponent % 10 + '0';
          new_exponent /= 10;
        }
        data->windex += new_exponent_width + 1;
      }
    }
  }
  if (number_end > data->rindex) {
    lighter_write_data(data, number_end - data->rindex);
  }
}

#endif /* LIGHTER_NUMBER_H */
