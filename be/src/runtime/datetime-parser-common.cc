// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "datetime-parser-common.h"

#include <boost/date_time/gregorian/gregorian.hpp>

#include "runtime/string-value.h"
#include "util/string-parser.h"

using std::string;
using std::unordered_set;

namespace impala {

namespace datetime_parse_util {

void DateTimeFormatContext::SetCenturyBreakAndCurrentTime(const TimestampValue& now) {
  current_time = &now;
  const boost::gregorian::date& now_date = now.date();
  // If the century break is at an invalid 02/29, set it to 02/28 for consistency with
  // Hive.
  if (now_date.month() == 2 && now_date.day() == 29 &&
      !boost::gregorian::gregorian_calendar::is_leap_year(now_date.year() - 80)) {
    century_break_ptime = boost::posix_time::ptime(
        boost::gregorian::date(now_date.year() - 80, 2, 28), now.time());
  } else {
    century_break_ptime = boost::posix_time::ptime(
        boost::gregorian::date(now_date.year() - 80, now_date.month(), now_date.day()),
        now.time());
  }
  DCHECK(!century_break_ptime.is_special());
}

void DateTimeFormatContext::Reset(const char* fmt, int fmt_len) {
  this->fmt = fmt;
  this->fmt_len = fmt_len;
  this->fmt_out_len = fmt_len;
  this->has_date_toks = false;
  this->has_time_toks = false;
  this->fx_modifier = false;
  this->toks.clear();
  this->century_break_ptime = boost::posix_time::not_a_date_time;
  this->current_time = nullptr;
}

void ReportBadFormat(FunctionContext* context, FormatTokenizationResult error_type,
    const StringVal& format, bool is_error) {
  DCHECK(context != nullptr);
  std::stringstream ss;
  if (format.is_null || format.len == 0) {
    ss << "Bad date/time conversion format: format string is NULL or has 0 length";
  } else {
    switch (error_type) {
      case DUPLICATE_FORMAT:
        ss << "PARSE ERROR: Invalid duplication of format element";
        break;
      case YEAR_WITH_ROUNDED_YEAR_ERROR:
        ss << "PARSE ERROR: Both year and round year are provided";
        break;
      case CONFLICTING_YEAR_TOKENS_ERROR:
        ss << "PARSE ERROR: Multiple year token provided";
        break;
      case DAY_OF_YEAR_TOKEN_CONFLICT:
        ss << "PARSE ERROR: Day of year provided with day or month token";
        break;
      case CONFLICTING_HOUR_TOKENS_ERROR:
        ss << "PARSE ERROR: Multiple hour tokens provided";
        break;
      case CONFLICTING_MERIDIEM_TOKENS_ERROR:
        ss << "PARSE ERROR: Multiple median indicator tokens provided";
        break;
      case MERIDIEM_CONFLICTS_WITH_HOUR_ERROR:
        ss << "PARSE ERROR: Conflict between median indicator and hour token";
        break;
      case MISSING_HOUR_TOKEN_ERROR:
        ss << "PARSE ERROR: Missing hour token";
        break;
      case SECOND_IN_DAY_CONFLICT:
        ss << "PARSE ERROR: Second of day token conflicts with other token(s)";
        break;
      case TOO_LONG_FORMAT_ERROR:
        ss << "PARSE ERROR: The input format is too long";
        break;
      case TIMEZONE_OFFSET_NOT_ALLOWED_ERROR:
        ss << "PARSE ERROR: Timezone offset not allowed in a datetime to string "
              "conversion";
        break;
      case MISSING_TZH_TOKEN_ERROR:
        ss << "PARSE ERROR: TZH token is required for TZM";
        break;
      case DATE_WITH_TIME_ERROR:
        ss << "PARSE ERROR: Time tokens provided with date type.";
        break;
      case CONFLICTING_FRACTIONAL_SECOND_TOKENS_ERROR:
        ss << "PARSE ERROR: Multiple fractional second token provided.";
        break;
      case TEXT_TOKEN_NOT_CLOSED:
        ss << "PARSE ERROR: Missing closing quotation mark.";
        break;
      case NO_DATETIME_TOKENS_ERROR:
        ss << "PARSE ERROR: No datetime tokens provided.";
        break;
      case MISPLACED_FX_MODIFIER_ERROR:
        ss << "PARSE ERROR: FX modifier should be at the beginning of the format string.";
        break;
      default:
        const StringValue& fmt = StringValue::FromStringVal(format);
        ss << "Bad date/time conversion format: " << fmt.DebugString();
    }
  }
  if (is_error) {
    context->SetError(ss.str().c_str());
  } else {
    context->AddWarning(ss.str().c_str());
  }
}

bool ParseAndValidate(const char* token, int token_len, int min, int max,
    int* result) {
  DCHECK(token != nullptr);
  DCHECK(token_len > 0);
  DCHECK(result != nullptr);
  StringParser::ParseResult status;
  *result = StringParser::StringToInt<int>(token, token_len, &status);
  if (UNLIKELY(StringParser::PARSE_SUCCESS != status)) return false;
  if (UNLIKELY(*result < min || *result > max)) return false;
  return true;
}

bool ParseFractionToken(const char* token, int token_len,
    DateTimeParseResult* result) {
  DCHECK(token != nullptr);
  DCHECK(token_len > 0);
  DCHECK(result != nullptr);
  StringParser::ParseResult status;
  result->fraction =
      StringParser::StringToInt<int32_t>(token, token_len, &status);
  if (UNLIKELY(StringParser::PARSE_SUCCESS != status)) return false;
  // A user may specify a time of 04:30:22.1238, the parser will return 1238 for
  // the fractional portion. This does not represent the intended value of
  // 123800000, therefore the number must be scaled up.
  if (token_len < FRACTIONAL_SECOND_MAX_LENGTH) {
    result->fraction *= std::pow(10, FRACTIONAL_SECOND_MAX_LENGTH - token_len);
  }
  return true;
}

int GetDayInYear(int year, int month, int day_in_month) {
  DCHECK(month >= 1 && month <= 12);
  const vector<int>& month_ranges = IsLeapYear(year) ? LEAP_YEAR_MONTH_RANGES :
      MONTH_RANGES;
  return day_in_month + month_ranges[month - 1];
}

bool GetMonthAndDayFromDaysSinceJan1(int year, int days_since_jan1, int* month,
    int* day) {
  DCHECK(days_since_jan1 >= 0 && days_since_jan1 < 366);
  DCHECK(month != nullptr);
  DCHECK(day != nullptr);
  // Calculate month using month ranges and the average month length.
  const vector<int>& month_ranges = IsLeapYear(year) ? LEAP_YEAR_MONTH_RANGES :
      MONTH_RANGES;
  int m = static_cast<int>(days_since_jan1 / 30.5);
  DCHECK(month_ranges[m] <= days_since_jan1);

  *month = (month_ranges[m + 1] <= days_since_jan1) ? m + 2 : m + 1;
  if (*month < 1 || *month > 12) return false;

  // Calculate day.
  *day = days_since_jan1 - month_ranges[*month - 1] + 1;
  return (*day >= 1 && *day <= 31);
}

string FormatTextToken(const DateTimeFormatToken& tok) {
  DCHECK(tok.type == TEXT);
  string result;
  result.reserve(tok.len);
  for (const char* text_it = tok.val; text_it < tok.val + tok.len; ++text_it) {
    if (*text_it != '\\') {
      result.append(text_it, 1);
      continue;
    }
    if (tok.is_double_escaped && strncmp(text_it, "\\\\\\\"", 4) == 0) {
      result.append("\"");
      text_it += 3;
    } else if (!tok.is_double_escaped && strncmp(text_it, "\\\"", 2) == 0) {
      result.append("\"");
      ++text_it;
    } else if (strncmp(text_it, "\\\\", 2) == 0) {
      result.append("\\");
      ++text_it;
    } else if (strncmp(text_it, "\\b", 2) == 0) {
      result.append("\b");
      ++text_it;
    } else if (strncmp(text_it, "\\n", 2) == 0) {
      result.append("\n");
      ++text_it;
    } else if (strncmp(text_it, "\\r", 2) == 0) {
      result.append("\r");
      ++text_it;
    } else if (strncmp(text_it, "\\t", 2) == 0) {
      result.append("\t");
      ++text_it;
    }
  }
  return result;
}


}
}
