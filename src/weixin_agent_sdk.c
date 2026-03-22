#include "weixin_agent_sdk.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sp.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#define WXA_DEFAULT_BASE_URL "https://ilinkai.weixin.qq.com"
#define WXA_DEFAULT_CDN_BASE_URL "https://novac2c.cdn.weixin.qq.com/c2c"
#define WXA_DEFAULT_BOT_TYPE "3"
#define WXA_DEFAULT_LONG_POLL_TIMEOUT_MS 35000U
#define WXA_DEFAULT_LOGIN_TIMEOUT_MS 480000U
#define WXA_DEFAULT_API_TIMEOUT_MS 15000L
#define WXA_SEND_API_TIMEOUT_MS 8000L
#define WXA_DEFAULT_CONFIG_TIMEOUT_MS 10000L
#define WXA_DEFAULT_CONNECT_TIMEOUT_MS 8000L
#define WXA_TYPING_STATUS_TYPING 1
#define WXA_TYPING_STATUS_CANCEL 2
#define WXA_SESSION_EXPIRED_ERRCODE (-14L)
#define WXA_SESSION_EXPIRED_PAUSE_MS (60U * 60U * 1000U)
#define WXA_MAX_BYTES (100U * 1024U * 1024U)
#define WXA_CURL_RETRY_MAX 3
#define WXA_SEND_MIN_INTERVAL_MS 350ULL
#define WXA_SEND_MAX_ATTEMPTS 2U
#define WXA_RETRY_QUEUE_MAX 128U
#define WXA_RETRY_QUEUE_MAX_ATTEMPTS 24U

struct wxa_client {
  sp_str_t base_url;
  sp_str_t cdn_base_url;
  sp_str_t bot_token;
  sp_str_t account_id;
  sp_str_t sync_buf;
  sp_str_t last_error;
  sp_str_t state_dir;
  sp_str_t media_dir;
  sp_str_t monitor_lock_path;
  wxa_log_fn log_fn;
  void* log_user_data;
  bool stop_requested;
  long last_seq;
  long last_message_id;
  sp_da(struct wxa_retry_send) retry_sends;
  unsigned long long last_send_at_ms;
};

typedef struct {
  sp_da(char) data;
} wxa_buffer_t;

typedef struct {
  long http_status;
  sp_str_t body;
  sp_str_t encrypted_param;
  sp_str_t error_message;
} wxa_http_response_t;

typedef struct {
  sp_str_t text;
  sp_str_t context_token;
  sp_str_t from_user_id;
  wxa_media_type_t media_type;
  sp_str_t media_encrypt_param;
  sp_str_t media_aes_key;
  sp_str_t media_hex_aes_key;
  sp_str_t media_file_name;
  sp_str_t media_mime_type;
} wxa_inbound_message_t;

typedef struct {
  sp_str_t download_param;
  sp_str_t aeskey_hex;
  u64 file_size_plain;
  u64 file_size_cipher;
} wxa_uploaded_media_t;

typedef enum {
  WXA_SEND_RESULT_OK = 0,
  WXA_SEND_RESULT_RETRYABLE = 1,
  WXA_SEND_RESULT_FATAL = 2
} wxa_send_result_t;

typedef struct wxa_retry_send {
  sp_str_t body;
  unsigned int attempts;
  unsigned long long next_retry_at_ms;
} wxa_retry_send_t;

typedef struct {
  int total;
  int failed;
} wxa_selftest_state_t;

static sp_str_t wxa_json_get_string(const char* body, const char* key);
static sp_str_t wxa_json_get_string_after_range(const char* body, const char* after, const char* key, const char* end);
static long wxa_json_get_long(const char* body, const char* key, bool* found);
static long wxa_json_get_long_range(const char* body, const char* key, bool* found, const char* end);
static sp_str_t wxa_printf(const char* fmt, ...);
static void wxa_free_str(sp_str_t* value);
static sp_str_t wxa_guess_mime(sp_str_t path_or_name);
static const char* wxa_skip_json_string(const char* cursor, const char* end);
static sp_str_t wxa_normalize_empty_string(sp_str_t value);
static void wxa_free_inbound_message(wxa_inbound_message_t* inbound);
static wxa_status_t wxa_parse_inbound_message(sp_str_t segment, wxa_inbound_message_t* inbound);
static sp_str_t wxa_body_from_message_item(sp_str_t item);
static void wxa_release_monitor_lock(wxa_client_t* client);
static void wxa_log_sync_state(wxa_client_t* client, const char* phase, sp_str_t sync_buf);
static unsigned int wxa_random_range_ms(unsigned int min_ms, unsigned int max_ms);
static sp_str_t wxa_build_send_text_body(const char* to_user_id, const char* context_token, const char* text);

static sp_str_t wxa_log_preview(sp_str_t value) {
  u32 preview_len = value.len < 48U ? value.len : 48U;
  if (preview_len == 0U || value.data == NULL) {
    return sp_str_lit("");
  }
  return sp_str_from_cstr_sized(value.data, preview_len);
}

static char* wxa_find_after_top_level_key(const char* body, const char* key, const char* end) {
  sp_str_t needle = sp_format("\"{}\"", SP_FMT_CSTR(key));
  const char* p = body;
  int object_depth = 0;
  char* match = NULL;

  while (p < end && *p != '\0') {
    if (*p == '"') {
      const char* string_end = wxa_skip_json_string(p, end);
      if (object_depth == 1 && p + (size_t)needle.len <= end &&
          strncmp(p, needle.data, (size_t)needle.len) == 0) {
        match = (char*)p;
        break;
      }
      p = string_end;
      continue;
    }
    if (*p == '{') {
      object_depth++;
    } else if (*p == '}') {
      object_depth--;
    }
    p++;
  }

  sp_free((void*)needle.data);
  if (match == NULL) {
    return NULL;
  }

  char* colon = match + needle.len;
  while (colon < end && *colon != '\0' && *colon != ':') {
    colon++;
  }
  if (colon >= end || *colon != ':') {
    return NULL;
  }
  return colon + 1;
}

static sp_str_t wxa_json_get_top_level_string(sp_str_t body, const char* key) {
  char* cursor = wxa_find_after_top_level_key(body.data, key, body.data + body.len);
  if (cursor == NULL) {
    return sp_str_lit("");
  }
  while (cursor < body.data + body.len && *cursor != '\0' && isspace((unsigned char)*cursor)) {
    cursor++;
  }
  if (cursor >= body.data + body.len || *cursor != '"') {
    return sp_str_lit("");
  }
  cursor++;
  sp_str_builder_t builder = SP_ZERO_INITIALIZE();
  while (cursor < body.data + body.len && *cursor != '\0') {
    if (*cursor == '"') {
      return wxa_normalize_empty_string(sp_str_builder_to_str(&builder));
    }
    if (*cursor == '\\') {
      cursor++;
      if (cursor >= body.data + body.len || *cursor == '\0') {
        return wxa_normalize_empty_string(sp_str_builder_to_str(&builder));
      }
      switch (*cursor) {
        case '"':
        case '\\':
        case '/': sp_str_builder_append_c8(&builder, *cursor); break;
        case 'b': sp_str_builder_append_c8(&builder, '\b'); break;
        case 'f': sp_str_builder_append_c8(&builder, '\f'); break;
        case 'n': sp_str_builder_append_c8(&builder, '\n'); break;
        case 'r': sp_str_builder_append_c8(&builder, '\r'); break;
        case 't': sp_str_builder_append_c8(&builder, '\t'); break;
        case 'u': cursor += 4; sp_str_builder_append_c8(&builder, '?'); break;
        default: sp_str_builder_append_c8(&builder, *cursor); break;
      }
    } else {
      sp_str_builder_append_c8(&builder, *cursor);
    }
    cursor++;
  }
  return wxa_normalize_empty_string(sp_str_builder_to_str(&builder));
}

static long wxa_json_get_top_level_long(sp_str_t body, const char* key, bool* found) {
  char* cursor = wxa_find_after_top_level_key(body.data, key, body.data + body.len);
  if (cursor == NULL) {
    if (found != NULL) {
      *found = false;
    }
    return 0L;
  }
  while (cursor < body.data + body.len && *cursor != '\0' && isspace((unsigned char)*cursor)) {
    cursor++;
  }
  char number_buf[64];
  size_t n = 0U;
  if (cursor < body.data + body.len && (*cursor == '-' || *cursor == '+')) {
    number_buf[n++] = *cursor++;
  }
  while (cursor < body.data + body.len && *cursor != '\0' && n + 1U < sizeof(number_buf) &&
         isdigit((unsigned char)*cursor)) {
    number_buf[n++] = *cursor++;
  }
  number_buf[n] = '\0';
  if (n == 0U || (n == 1U && (number_buf[0] == '-' || number_buf[0] == '+'))) {
    if (found != NULL) {
      *found = false;
    }
    return 0L;
  }
  if (found != NULL) {
    *found = true;
  }
  return strtol(number_buf, NULL, 10);
}

static char* wxa_find_top_level_array(sp_str_t body, const char* key) {
  char* after = wxa_find_after_top_level_key(body.data, key, body.data + body.len);
  if (after == NULL) {
    return NULL;
  }
  for (char* p = after; p < body.data + body.len && *p != '\0'; ++p) {
    if (*p == '"') {
      const char* next = wxa_skip_json_string(p, body.data + body.len);
      p = (char*)(next - 1);
      continue;
    }
    if (*p == '[') {
      return p;
    }
    if (*p == ',' || *p == '}') {
      break;
    }
  }
  return NULL;
}

static void wxa_sleep_ms(unsigned int ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  struct timespec req = {
    .tv_sec = (time_t)(ms / 1000U),
    .tv_nsec = (long)(ms % 1000U) * 1000000L
  };
  while (nanosleep(&req, &req) != 0 && errno == EINTR) {
  }
#endif
}

static unsigned long long wxa_now_millis(void) {
#ifdef _WIN32
  return (unsigned long long)GetTickCount64();
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return (unsigned long long)time(NULL) * 1000ULL;
  }
  return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000L);
#endif
}

static unsigned long long wxa_next_client_nonce(void) {
  static unsigned long long nonce = 0ULL;
  nonce++;
  if (nonce == 0ULL) {
    nonce = 1ULL;
  }
  return nonce;
}

static sp_str_t wxa_make_client_id(void) {
  return wxa_printf(
    "c-%llu-%llu",
    wxa_now_millis(),
    wxa_next_client_nonce()
  );
}

static bool wxa_is_retryable_send_error(wxa_status_t transport_status, long ret, long errcode) {
  if (transport_status == WXA_ERR_NETWORK || transport_status == WXA_ERR_TIMEOUT) {
    return true;
  }
  return ret == -1L || errcode == -1L;
}

static unsigned int wxa_send_retry_delay_ms(unsigned int attempts) {
  unsigned int delay = attempts * 500U;
  if (delay < 500U) {
    delay = 500U;
  }
  if (delay > 5000U) {
    delay = 5000U;
  }
  return delay;
}

static unsigned int wxa_retry_queue_delay_ms(unsigned int attempts) {
  if (attempts <= 1U) {
    return wxa_random_range_ms(60U, 140U);
  }
  if (attempts <= 3U) {
    return wxa_random_range_ms(120U, 260U);
  }
  if (attempts <= 8U) {
    return wxa_random_range_ms(220U, 500U);
  }
  return wxa_random_range_ms(450U, 900U);
}

static bool wxa_refresh_client_id_in_send_body(sp_str_t* body) {
  if (body == NULL || body->data == NULL || body->len == 0U) {
    return false;
  }

  const char* marker = "\"client_id\":\"";
  char* pos = strstr(body->data, marker);
  if (pos == NULL) {
    return false;
  }
  char* value_start = pos + strlen(marker);
  char* value_end = strchr(value_start, '"');
  if (value_end == NULL || value_end <= value_start) {
    return false;
  }

  sp_str_t new_id = wxa_make_client_id();
  sp_str_t prefix = sp_str_from_cstr_sized(body->data, (u32)(value_start - body->data));
  sp_str_t suffix = sp_str_from_cstr(value_end);
  sp_str_t replaced = sp_format("{}{}{}", SP_FMT_STR(prefix), SP_FMT_STR(new_id), SP_FMT_STR(suffix));
  wxa_free_str(&prefix);
  wxa_free_str(&suffix);
  wxa_free_str(&new_id);
  wxa_free_str(body);
  *body = replaced;
  return true;
}

static void wxa_log(wxa_client_t* client, const char* message) {
  if (client != NULL && client->log_fn != NULL) {
    client->log_fn(client->log_user_data, message);
    return;
  }
  fputs(message, stderr);
  fputc('\n', stderr);
}

static bool wxa_curl_init_once(void) {
  static bool initialized = false;
  static bool ok = false;
  if (!initialized) {
    initialized = true;
    ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  }
  return ok;
}

static sp_str_t wxa_normalize_empty_string(sp_str_t value) {
  if (value.len == 0U) {
    if (value.data != NULL) {
      sp_free((void*)value.data);
    }
    return sp_str_lit("");
  }
  return value;
}

static sp_str_t wxa_printf(const char* fmt, ...) {
  va_list args;
  va_list copy;
  int needed;
  char* buf;
  va_start(args, fmt);
  va_copy(copy, args);
  needed = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (needed < 0) {
    va_end(args);
    return sp_str_lit("");
  }
  buf = sp_alloc((u32)needed + 1U);
  vsnprintf(buf, (size_t)needed + 1U, fmt, args);
  va_end(args);
  return sp_str(buf, (u32)needed);
}

static void wxa_free_str(sp_str_t* value) {
  if (value != NULL && value->data != NULL && value->len > 0U) {
    sp_free((void*)value->data);
  }
  if (value != NULL) {
    *value = sp_str_lit("");
  }
}

static void wxa_set_error(wxa_client_t* client, const char* message) {
  if (client == NULL) {
    return;
  }
  wxa_free_str(&client->last_error);
  client->last_error = sp_str_from_cstr(message != NULL ? message : "unknown error");
}

static wxa_status_t wxa_fail(wxa_client_t* client, wxa_status_t status, const char* message) {
  wxa_set_error(client, message);
  return status;
}

static sp_str_t wxa_copy_or_empty(const char* value) {
  if (value == NULL) {
    return sp_str_lit("");
  }
  return sp_str_from_cstr(value);
}

static const char* wxa_status_message(wxa_status_t status) {
  switch (status) {
    case WXA_OK: {
      return "ok";
    }
    case WXA_ERR_ARGUMENT: {
      return "invalid argument";
    }
    case WXA_ERR_NETWORK: {
      return "network error";
    }
    case WXA_ERR_PROTOCOL: {
      return "protocol error";
    }
    case WXA_ERR_TIMEOUT: {
      return "timeout";
    }
    case WXA_ERR_CALLBACK: {
      return "agent callback failed";
    }
  }
  return "unknown";
}

static size_t wxa_write_cb(char* ptr, size_t size, size_t nmemb, void* user_data) {
  size_t total = size * nmemb;
  wxa_buffer_t* buffer = (wxa_buffer_t*)user_data;
  if (buffer == NULL || total == 0U) {
    return total;
  }

  for (size_t i = 0; i < total; ++i) {
    sp_dyn_array_push(buffer->data, ptr[i]);
  }
  return total;
}

static sp_str_t wxa_buffer_to_str(wxa_buffer_t* buffer) {
  if (buffer == NULL || sp_dyn_array_size(buffer->data) == 0U) {
    return sp_str_lit("");
  }

  sp_dyn_array_push(buffer->data, '\0');
  return wxa_normalize_empty_string(sp_str_from_cstr(buffer->data));
}

static sp_str_t wxa_normalize_account_id(sp_str_t raw) {
  sp_str_t copy = sp_str_copy(raw);
  for (u32 i = 0; i < copy.len; ++i) {
    c8 c = copy.data[i];
    if (c == '@' || c == '.') {
      ((c8*)copy.data)[i] = '-';
    } else {
      ((c8*)copy.data)[i] = (c8)tolower((unsigned char)c);
    }
  }
  return copy;
}

static sp_str_t wxa_json_escape(const char* input) {
  sp_str_builder_t builder = SP_ZERO_INITIALIZE();
  sp_str_t view = sp_str_view(input != NULL ? input : "");

  sp_str_for(view, i) {
    c8 c = view.data[i];
    switch (c) {
      case '\\': {
        sp_str_builder_append_cstr(&builder, "\\\\");
        break;
      }
      case '"': {
        sp_str_builder_append_cstr(&builder, "\\\"");
        break;
      }
      case '\n': {
        sp_str_builder_append_cstr(&builder, "\\n");
        break;
      }
      case '\r': {
        sp_str_builder_append_cstr(&builder, "\\r");
        break;
      }
      case '\t': {
        sp_str_builder_append_cstr(&builder, "\\t");
        break;
      }
      default: {
        sp_str_builder_append_c8(&builder, c);
        break;
      }
    }
  }

  return wxa_normalize_empty_string(sp_str_builder_to_str(&builder));
}

static char* wxa_find_after_key_from_range(const char* body, const char* key, const char* start_from, const char* end) {
  sp_str_t needle = sp_format("\"{}\"", SP_FMT_CSTR(key));
  const char* search_from = start_from != NULL ? start_from : body;
  size_t needle_len = (size_t)needle.len;
  char* match = NULL;
  for (const char* p = search_from; p != NULL && p + needle_len <= end && *p != '\0'; ++p) {
    if (strncmp(p, needle.data, needle_len) == 0) {
      match = (char*)p;
      break;
    }
    if (*p == '"') {
      const char* next = wxa_skip_json_string(p, end);
      if (next > p + 1) {
        p = next - 1;
        continue;
      }
    }
  }
  sp_free((void*)needle.data);
  if (match == NULL) {
    return NULL;
  }

  char* colon = match + needle_len;
  while (colon < end && *colon != ':' && *colon != '\0') {
    if (*colon == '"') {
      const char* next = wxa_skip_json_string(colon, end);
      colon = (char*)next;
      continue;
    }
    colon++;
  }
  if (colon >= end || *colon != ':') {
    return NULL;
  }
  return colon + 1;
}

static char* wxa_find_after_key_from(const char* body, const char* key, const char* start_from) {
  return wxa_find_after_key_from_range(body, key, start_from, body + strlen(body));
}

static char* wxa_find_after_key(const char* body, const char* key) {
  return wxa_find_after_key_from(body, key, body);
}

static char* wxa_find_object_in_range(const char* body, const char* key, const char* end) {
  char* after = wxa_find_after_key_from_range(body, key, body, end);
  if (after == NULL) {
    return NULL;
  }
  for (char* p = after; p < end && *p != '\0'; ++p) {
    if (*p == '"') {
      const char* next = wxa_skip_json_string(p, end);
      p = (char*)(next - 1);
      continue;
    }
    if (*p == '{') {
      return p;
    }
    if (*p == ',' || *p == '}') {
      break;
    }
  }
  return NULL;
}

static char* wxa_find_array_in_range(const char* body, const char* key, const char* end) {
  char* after = wxa_find_after_key_from_range(body, key, body, end);
  if (after == NULL) {
    return NULL;
  }
  for (char* p = after; p < end && *p != '\0'; ++p) {
    if (*p == '"') {
      const char* next = wxa_skip_json_string(p, end);
      p = (char*)(next - 1);
      continue;
    }
    if (*p == '[') {
      return p;
    }
    if (*p == ',' || *p == '}') {
      break;
    }
  }
  return NULL;
}

static char* wxa_find_array(const char* body, const char* key) {
  return wxa_find_array_in_range(body, key, body + strlen(body));
}

static const char* wxa_skip_json_string(const char* cursor, const char* end) {
  const char* p = cursor;
  if (p == NULL || p >= end || *p != '"') {
    return cursor;
  }
  p++;
  while (p < end) {
    if (*p == '\\') {
      p += (p + 1 < end) ? 2 : 1;
      continue;
    }
    if (*p == '"') {
      return p + 1;
    }
    p++;
  }
  return end;
}

static const char* wxa_find_matching_json(const char* start, const char* end, char open_ch, char close_ch) {
  int depth = 0;
  const char* p = start;
  while (p < end && *p != '\0') {
    if (*p == '"') {
      p = wxa_skip_json_string(p, end);
      continue;
    }
    if (*p == open_ch) {
      depth++;
    } else if (*p == close_ch) {
      depth--;
      if (depth == 0) {
        return p;
      }
    }
    p++;
  }
  return NULL;
}

static bool wxa_json_next_object(const char** cursor, const char* end, sp_str_t* out) {
  const char* p = *cursor;
  while (p < end && *p != '{') {
    if (*p == '"') {
      p = wxa_skip_json_string(p, end);
      continue;
    }
    p++;
  }
  if (p >= end || *p != '{') {
    return false;
  }
  const char* close = wxa_find_matching_json(p, end, '{', '}');
  if (close == NULL) {
    return false;
  }
  *out = sp_str(p, (u32)(close - p + 1));
  *cursor = close + 1;
  return true;
}

static sp_str_t wxa_json_object_slice(const char* object_start, const char* end_limit) {
  if (object_start == NULL) {
    return sp_str_lit("");
  }
  const char* close = wxa_find_matching_json(object_start, end_limit, '{', '}');
  if (close == NULL) {
    return sp_str_lit("");
  }
  return sp_str(object_start, (u32)(close - object_start + 1));
}

static void wxa_append_text_line(sp_str_t* acc, sp_str_t line) {
  if (acc == NULL || line.len == 0U) {
    return;
  }
  if (acc->len == 0U) {
    *acc = sp_str_copy(line);
    return;
  }
  sp_str_t next = sp_format("{}\n{}", SP_FMT_STR(*acc), SP_FMT_STR(line));
  wxa_free_str(acc);
  *acc = next;
}

static void wxa_try_extract_media_from_item(
  sp_str_t item,
  wxa_inbound_message_t* inbound,
  bool allow_overwrite
) {
  const char* item_end = item.data + item.len;
  if (inbound == NULL) {
    return;
  }
  if (!allow_overwrite && inbound->media_type != WXA_MEDIA_NONE) {
    return;
  }

  bool found_type = false;
  long item_type = wxa_json_get_long_range(item.data, "type", &found_type, item_end);
  if (!found_type) {
    return;
  }

  if (item_type == 2L) {
    char* image_item = wxa_find_object_in_range(item.data, "image_item", item_end);
    if (image_item != NULL) {
      inbound->media_type = WXA_MEDIA_IMAGE;
      wxa_free_str(&inbound->media_encrypt_param);
      wxa_free_str(&inbound->media_aes_key);
      wxa_free_str(&inbound->media_hex_aes_key);
      inbound->media_encrypt_param = wxa_json_get_string_after_range(item.data, image_item, "encrypt_query_param", item_end);
      inbound->media_aes_key = wxa_json_get_string_after_range(item.data, image_item, "aes_key", item_end);
      inbound->media_hex_aes_key = wxa_json_get_string_after_range(item.data, image_item, "aeskey", item_end);
      inbound->media_mime_type = sp_str_lit("image/jpeg");
    }
  } else if (item_type == 5L) {
    char* video_item = wxa_find_object_in_range(item.data, "video_item", item_end);
    if (video_item != NULL) {
      inbound->media_type = WXA_MEDIA_VIDEO;
      wxa_free_str(&inbound->media_encrypt_param);
      wxa_free_str(&inbound->media_aes_key);
      wxa_free_str(&inbound->media_hex_aes_key);
      inbound->media_encrypt_param = wxa_json_get_string_after_range(item.data, video_item, "encrypt_query_param", item_end);
      inbound->media_aes_key = wxa_json_get_string_after_range(item.data, video_item, "aes_key", item_end);
      inbound->media_mime_type = sp_str_lit("video/mp4");
    }
  } else if (item_type == 4L) {
    char* file_item = wxa_find_object_in_range(item.data, "file_item", item_end);
    if (file_item != NULL) {
      inbound->media_type = WXA_MEDIA_FILE;
      wxa_free_str(&inbound->media_encrypt_param);
      wxa_free_str(&inbound->media_aes_key);
      wxa_free_str(&inbound->media_hex_aes_key);
      wxa_free_str(&inbound->media_file_name);
      inbound->media_encrypt_param = wxa_json_get_string_after_range(item.data, file_item, "encrypt_query_param", item_end);
      inbound->media_aes_key = wxa_json_get_string_after_range(item.data, file_item, "aes_key", item_end);
      inbound->media_file_name = wxa_json_get_string_after_range(item.data, file_item, "file_name", item_end);
      inbound->media_mime_type = wxa_guess_mime(inbound->media_file_name);
    }
  } else if (item_type == 3L) {
    char* voice_item = wxa_find_object_in_range(item.data, "voice_item", item_end);
    if (voice_item != NULL) {
      sp_str_t voice_text = wxa_json_get_string_after_range(item.data, voice_item, "text", item_end);
      if (voice_text.len == 0U) {
        inbound->media_type = WXA_MEDIA_AUDIO;
        wxa_free_str(&inbound->media_encrypt_param);
        wxa_free_str(&inbound->media_aes_key);
        wxa_free_str(&inbound->media_hex_aes_key);
        inbound->media_encrypt_param = wxa_json_get_string_after_range(item.data, voice_item, "encrypt_query_param", item_end);
        inbound->media_aes_key = wxa_json_get_string_after_range(item.data, voice_item, "aes_key", item_end);
        inbound->media_mime_type = sp_str_lit("audio/silk");
      }
      if (voice_text.data != NULL) {
        sp_free((void*)voice_text.data);
      }
    }
  }
}

static void wxa_free_inbound_message(wxa_inbound_message_t* inbound) {
  if (inbound == NULL) {
    return;
  }
  wxa_free_str(&inbound->text);
  wxa_free_str(&inbound->context_token);
  wxa_free_str(&inbound->from_user_id);
  wxa_free_str(&inbound->media_encrypt_param);
  wxa_free_str(&inbound->media_aes_key);
  wxa_free_str(&inbound->media_hex_aes_key);
  wxa_free_str(&inbound->media_file_name);
}

static sp_str_t wxa_body_from_message_item(sp_str_t item) {
  const char* item_end = item.data + item.len;
  if (item.len == 0U) {
    return sp_str_lit("");
  }

  bool found_type = false;
  long item_type = wxa_json_get_long_range(item.data, "type", &found_type, item_end);
  if (!found_type) {
    return sp_str_lit("");
  }

  if (item_type == 1L) {
    char* text_item = wxa_find_object_in_range(item.data, "text_item", item_end);
    sp_str_t text = sp_str_lit("");
    if (text_item != NULL) {
      text = wxa_json_get_string_after_range(item.data, text_item, "text", item_end);
    }

    char* ref_msg = wxa_find_object_in_range(item.data, "ref_msg", item_end);
    if (ref_msg != NULL) {
      char* ref_message_item = wxa_find_object_in_range(ref_msg, "message_item", item_end);
      if (ref_message_item != NULL) {
        sp_str_t ref_item_slice = wxa_json_object_slice(ref_message_item, item.data + item.len);
        if (ref_item_slice.len > 0U) {
          bool ref_found_type = false;
          long ref_type = wxa_json_get_long_range(ref_item_slice.data, "type", &ref_found_type, ref_item_slice.data + ref_item_slice.len);
          if (ref_found_type && (ref_type == 2L || ref_type == 3L || ref_type == 4L || ref_type == 5L)) {
            return text;
          }
        }
      }

      sp_str_t title = wxa_json_get_string_after_range(item.data, ref_msg, "title", item_end);
      sp_str_t ref_text = sp_str_lit("");
      sp_str_t body = sp_str_lit("");
      if (ref_message_item != NULL) {
        sp_str_t ref_item_slice = wxa_json_object_slice(ref_message_item, item.data + item.len);
        if (ref_item_slice.len > 0U) {
          ref_text = wxa_body_from_message_item(ref_item_slice);
        }
      }

      if (title.len == 0U && ref_text.len == 0U) {
        wxa_free_str(&title);
        wxa_free_str(&ref_text);
        return text;
      }

      if (title.len > 0U && ref_text.len > 0U) {
        body = sp_format("[引用: {} | {}]\n{}", SP_FMT_STR(title), SP_FMT_STR(ref_text), SP_FMT_STR(text));
      } else if (title.len > 0U) {
        body = sp_format("[引用: {}]\n{}", SP_FMT_STR(title), SP_FMT_STR(text));
      } else {
        body = sp_format("[引用: {}]\n{}", SP_FMT_STR(ref_text), SP_FMT_STR(text));
      }
      wxa_free_str(&title);
      wxa_free_str(&ref_text);
      wxa_free_str(&text);
      return body;
    }
    return text;
  }

  if (item_type == 3L) {
    char* voice_item = wxa_find_object_in_range(item.data, "voice_item", item_end);
    if (voice_item != NULL) {
      sp_str_t voice_text = wxa_json_get_string_after_range(item.data, voice_item, "text", item_end);
      return voice_text;
    }
  }

  return sp_str_lit("");
}

static bool wxa_extract_downloadable_media_from_item(
  sp_str_t item,
  wxa_inbound_message_t* inbound
) {
  if (inbound == NULL) {
    return false;
  }

  wxa_inbound_message_t candidate = {0};
  wxa_try_extract_media_from_item(item, &candidate, true);
  if (candidate.media_type == WXA_MEDIA_AUDIO) {
    char* voice_item = wxa_find_object_in_range(item.data, "voice_item", item.data + item.len);
    if (voice_item != NULL) {
      sp_str_t voice_text = wxa_json_get_string_after_range(item.data, voice_item, "text", item.data + item.len);
      if (voice_text.len > 0U) {
        wxa_free_str(&voice_text);
        wxa_free_inbound_message(&candidate);
        return false;
      }
      wxa_free_str(&voice_text);
    }
  }

  if (candidate.media_type == WXA_MEDIA_NONE || candidate.media_encrypt_param.len == 0U) {
    wxa_free_inbound_message(&candidate);
    return false;
  }

  wxa_free_str(&inbound->media_encrypt_param);
  wxa_free_str(&inbound->media_aes_key);
  wxa_free_str(&inbound->media_hex_aes_key);
  wxa_free_str(&inbound->media_file_name);

  inbound->media_type = candidate.media_type;
  inbound->media_encrypt_param = candidate.media_encrypt_param;
  inbound->media_aes_key = candidate.media_aes_key;
  inbound->media_hex_aes_key = candidate.media_hex_aes_key;
  inbound->media_file_name = candidate.media_file_name;
  inbound->media_mime_type = candidate.media_mime_type;
  return true;
}

static bool wxa_find_direct_media_in_list(
  const char* cursor_start,
  const char* list_end,
  long target_type,
  wxa_inbound_message_t* inbound
) {
  const char* cursor = cursor_start;
  while (cursor < list_end) {
    sp_str_t item = sp_str_lit("");
    bool found_type = false;
    long item_type = 0L;
    if (!wxa_json_next_object(&cursor, list_end, &item)) {
      break;
    }
    item_type = wxa_json_get_long_range(item.data, "type", &found_type, item.data + item.len);
    if (!found_type || item_type != target_type) {
      continue;
    }
    if (wxa_extract_downloadable_media_from_item(item, inbound)) {
      return true;
    }
  }
  return false;
}

static void wxa_extract_media_from_item_list(
  const char* list_start,
  const char* list_end,
  wxa_inbound_message_t* inbound
) {
  if (list_start == NULL || list_end == NULL || inbound == NULL) {
    return;
  }

  if (wxa_find_direct_media_in_list(list_start, list_end, 2L, inbound) ||
      wxa_find_direct_media_in_list(list_start, list_end, 5L, inbound) ||
      wxa_find_direct_media_in_list(list_start, list_end, 4L, inbound) ||
      wxa_find_direct_media_in_list(list_start, list_end, 3L, inbound)) {
    return;
  }

  const char* cursor = list_start;
  while (cursor < list_end) {
    sp_str_t item = sp_str_lit("");
    bool found_type = false;
    long item_type = 0L;
    if (!wxa_json_next_object(&cursor, list_end, &item)) {
      break;
    }
    item_type = wxa_json_get_long_range(item.data, "type", &found_type, item.data + item.len);
    if (!found_type || item_type != 1L) {
      continue;
    }
    char* ref_msg = wxa_find_object_in_range(item.data, "ref_msg", item.data + item.len);
    char* ref_message_item = ref_msg != NULL ? wxa_find_object_in_range(ref_msg, "message_item", item.data + item.len) : NULL;
    if (ref_message_item == NULL) {
      continue;
    }
    sp_str_t ref_item_slice = wxa_json_object_slice(ref_message_item, item.data + item.len);
    if (ref_item_slice.len == 0U) {
      continue;
    }
    if (wxa_extract_downloadable_media_from_item(ref_item_slice, inbound)) {
      return;
    }
  }
}

static wxa_status_t wxa_parse_inbound_message(sp_str_t segment, wxa_inbound_message_t* inbound) {
  if (inbound == NULL) {
    return WXA_ERR_ARGUMENT;
  }

  *inbound = (wxa_inbound_message_t){0};
  inbound->from_user_id = wxa_json_get_string(segment.data, "from_user_id");
  inbound->context_token = wxa_json_get_string(segment.data, "context_token");

  char* item_list = wxa_find_array(segment.data, "item_list");
  if (item_list != NULL) {
    const char* list_end = wxa_find_matching_json(item_list, segment.data + segment.len, '[', ']');
    const char* cursor = item_list + 1;
    while (list_end != NULL) {
      sp_str_t item = sp_str_lit("");
      if (!wxa_json_next_object(&cursor, list_end, &item)) {
        break;
      }
      sp_str_t text = wxa_body_from_message_item(item);
      if (text.len > 0U) {
        inbound->text = text;
        break;
      }
    }
    if (list_end != NULL) {
      wxa_extract_media_from_item_list(item_list + 1, list_end, inbound);
    }
  }

  return WXA_OK;
}

static sp_str_t wxa_json_get_string_after_range(const char* body, const char* after, const char* key, const char* end) {
  char* cursor = wxa_find_after_key_from_range(body, key, after, end);
  if (cursor == NULL) {
    return sp_str_lit("");
  }
  while (cursor < end && *cursor != '\0' && isspace((unsigned char)*cursor)) {
    cursor++;
  }
  if (cursor >= end || *cursor != '"') {
    return sp_str_lit("");
  }
  const char* p = cursor + 1;
  sp_str_builder_t builder = SP_ZERO_INITIALIZE();
  while (p < end && *p != '\0') {
    if (*p == '"') {
      return wxa_normalize_empty_string(sp_str_builder_to_str(&builder));
    }
    if (*p == '\\') {
      p++;
      if (p >= end || *p == '\0') {
        return sp_str_builder_to_str(&builder);
      }
      switch (*p) {
        case '"':
        case '\\':
        case '/': sp_str_builder_append_c8(&builder, *p); break;
        case 'b': sp_str_builder_append_c8(&builder, '\b'); break;
        case 'f': sp_str_builder_append_c8(&builder, '\f'); break;
        case 'n': sp_str_builder_append_c8(&builder, '\n'); break;
        case 'r': sp_str_builder_append_c8(&builder, '\r'); break;
        case 't': sp_str_builder_append_c8(&builder, '\t'); break;
        case 'u': p += 4; sp_str_builder_append_c8(&builder, '?'); break;
        default: sp_str_builder_append_c8(&builder, *p); break;
      }
    } else {
      sp_str_builder_append_c8(&builder, *p);
    }
    p++;
  }
  return sp_str_builder_to_str(&builder);
}

static sp_str_t wxa_json_get_string(const char* body, const char* key) {
  char* cursor = wxa_find_after_key(body, key);
  if (cursor == NULL) {
    return sp_str_lit("");
  }

  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    cursor++;
  }
  if (*cursor != '"') {
    return sp_str_lit("");
  }
  cursor++;

  sp_str_builder_t builder = SP_ZERO_INITIALIZE();
  while (*cursor != '\0') {
    if (*cursor == '"') {
      return sp_str_builder_to_str(&builder);
    }
    if (*cursor == '\\') {
      cursor++;
      switch (*cursor) {
        case '"':
        case '\\':
        case '/': {
          sp_str_builder_append_c8(&builder, *cursor);
          break;
        }
        case 'b': {
          sp_str_builder_append_c8(&builder, '\b');
          break;
        }
        case 'f': {
          sp_str_builder_append_c8(&builder, '\f');
          break;
        }
        case 'n': {
          sp_str_builder_append_c8(&builder, '\n');
          break;
        }
        case 'r': {
          sp_str_builder_append_c8(&builder, '\r');
          break;
        }
        case 't': {
          sp_str_builder_append_c8(&builder, '\t');
          break;
        }
        case 'u': {
          cursor += 4;
          sp_str_builder_append_c8(&builder, '?');
          break;
        }
        case '\0': {
          return wxa_normalize_empty_string(sp_str_builder_to_str(&builder));
        }
        default: {
          sp_str_builder_append_c8(&builder, *cursor);
          break;
        }
      }
    } else {
      sp_str_builder_append_c8(&builder, *cursor);
    }
    cursor++;
  }

  return wxa_normalize_empty_string(sp_str_builder_to_str(&builder));
}

static long wxa_json_get_long(const char* body, const char* key, bool* found) {
  return wxa_json_get_long_range(body, key, found, body + strlen(body));
}

static long wxa_json_get_long_range(const char* body, const char* key, bool* found, const char* end) {
  char* cursor = wxa_find_after_key_from_range(body, key, body, end);
  if (cursor == NULL) {
    if (found != NULL) {
      *found = false;
    }
    return 0L;
  }
  while (cursor < end && *cursor != '\0' && isspace((unsigned char)*cursor)) {
    cursor++;
  }
  char number_buf[64];
  size_t n = 0U;
  if (cursor < end && (*cursor == '-' || *cursor == '+')) {
    number_buf[n++] = *cursor++;
  }
  while (cursor < end && *cursor != '\0' && n + 1U < sizeof(number_buf) &&
         isdigit((unsigned char)*cursor)) {
    number_buf[n++] = *cursor++;
  }
  number_buf[n] = '\0';
  if (n == 0U || (n == 1U && (number_buf[0] == '-' || number_buf[0] == '+'))) {
    if (found != NULL) {
      *found = false;
    }
    return 0L;
  }
  long value = strtol(number_buf, NULL, 10);
  if (found != NULL) {
    *found = true;
  }
  return value;
}

static sp_str_t wxa_build_url(sp_str_t base_url, const char* suffix) {
  if (base_url.len == 0U) {
    return sp_str_from_cstr(suffix);
  }
  if (base_url.data[base_url.len - 1] == '/') {
    return sp_format("{}{}", SP_FMT_STR(base_url), SP_FMT_CSTR(suffix));
  }
  return sp_format("{}/{}", SP_FMT_STR(base_url), SP_FMT_CSTR(suffix));
}

static sp_str_t wxa_join_path2(sp_str_t a, const char* b) {
  if (a.len == 0U) {
    return sp_str_from_cstr(b);
  }
  if (a.data[a.len - 1] == '/') {
    return sp_format("{}{}", SP_FMT_STR(a), SP_FMT_CSTR(b));
  }
  return sp_format("{}/{}", SP_FMT_STR(a), SP_FMT_CSTR(b));
}

static bool wxa_ensure_dir(sp_str_t path) {
  char* cpath = sp_str_to_cstr(path);
  size_t len = strlen(cpath);
  for (size_t i = 1; i < len; ++i) {
    if (cpath[i] == '/') {
      cpath[i] = '\0';
      if (mkdir(cpath, 0700) != 0 && errno != EEXIST) {
        sp_free(cpath);
        return false;
      }
      cpath[i] = '/';
    }
  }
  if (mkdir(cpath, 0700) != 0 && errno != EEXIST) {
    sp_free(cpath);
    return false;
  }
  sp_free(cpath);
  return true;
}

static bool wxa_write_text_file(sp_str_t path, sp_str_t text) {
  char* file_path = sp_str_to_cstr(path);
  FILE* fp = fopen(file_path, "wb");
  if (fp == NULL) {
    sp_free(file_path);
    return false;
  }
  bool ok = fwrite(text.data, 1U, text.len, fp) == text.len;
  fclose(fp);
  chmod(file_path, 0600);
  sp_free(file_path);
  return ok;
}

static sp_str_t wxa_read_text_file(sp_str_t path) {
  char* file_path = sp_str_to_cstr(path);
  FILE* fp = fopen(file_path, "rb");
  sp_free(file_path);
  if (fp == NULL) {
    return sp_str_lit("");
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    fclose(fp);
    return sp_str_lit("");
  }
  long sz = ftell(fp);
  if (sz < 0L) {
    fclose(fp);
    return sp_str_lit("");
  }
  rewind(fp);
  char* buf = sp_alloc((u32)sz + 1U);
  size_t got = fread(buf, 1U, (size_t)sz, fp);
  fclose(fp);
  if (got == 0U) {
    sp_free(buf);
    return sp_str_lit("");
  }
  buf[got] = '\0';
  return sp_str(buf, (u32)got);
}

static sp_str_t wxa_resolve_state_dir(void) {
  const char* state_env = getenv("OPENCLAW_STATE_DIR");
  if (state_env != NULL && state_env[0] != '\0') {
    return sp_str_from_cstr(state_env);
  }
  state_env = getenv("CLAWDBOT_STATE_DIR");
  if (state_env != NULL && state_env[0] != '\0') {
    return sp_str_from_cstr(state_env);
  }
  const char* home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    return sp_str_from_cstr(".openclaw");
  }
  sp_str_t base = sp_str_from_cstr(home);
  sp_str_t state = wxa_join_path2(base, ".openclaw");
  sp_free((void*)base.data);
  return state;
}

static sp_str_t wxa_accounts_root(wxa_client_t* client) {
  sp_str_t openclaw_weixin = wxa_join_path2(client->state_dir, "openclaw-weixin");
  sp_str_t accounts = wxa_join_path2(openclaw_weixin, "accounts");
  sp_free((void*)openclaw_weixin.data);
  return accounts;
}

static sp_str_t wxa_account_json_path(wxa_client_t* client, sp_str_t account_id) {
  sp_str_t root = wxa_accounts_root(client);
  sp_str_t path = sp_format("{}/{}.json", SP_FMT_STR(root), SP_FMT_STR(account_id));
  sp_free((void*)root.data);
  return path;
}

static sp_str_t wxa_sync_buf_path(wxa_client_t* client, sp_str_t account_id) {
  sp_str_t root = wxa_accounts_root(client);
  sp_str_t path = sp_format("{}/{}.sync.json", SP_FMT_STR(root), SP_FMT_STR(account_id));
  sp_free((void*)root.data);
  return path;
}

static sp_str_t wxa_accounts_index_path(wxa_client_t* client) {
  sp_str_t root = wxa_join_path2(client->state_dir, "openclaw-weixin");
  sp_str_t path = wxa_join_path2(root, "accounts.json");
  sp_free((void*)root.data);
  return path;
}

static sp_str_t wxa_monitor_lock_path(wxa_client_t* client, sp_str_t account_id) {
  sp_str_t root = wxa_accounts_root(client);
  sp_str_t path = sp_format("{}/{}.monitor.lock", SP_FMT_STR(root), SP_FMT_STR(account_id));
  sp_free((void*)root.data);
  return path;
}

static void wxa_save_account_record(wxa_client_t* client, sp_str_t user_id) {
  if (client->account_id.len == 0U || client->bot_token.len == 0U) {
    return;
  }
  sp_str_t accounts_root = wxa_accounts_root(client);
  if (!wxa_ensure_dir(accounts_root)) {
    sp_free((void*)accounts_root.data);
    return;
  }
  sp_free((void*)accounts_root.data);

  char* token = sp_str_to_cstr(client->bot_token);
  char* base_url = sp_str_to_cstr(client->base_url);
  char* user = sp_str_to_cstr(user_id);
  sp_str_t payload = wxa_printf(
    "{\"token\":\"%s\",\"baseUrl\":\"%s\",\"userId\":\"%s\"}",
    token,
    base_url,
    user
  );
  sp_str_t account_path = wxa_account_json_path(client, client->account_id);
  (void)wxa_write_text_file(account_path, payload);
  sp_free(token);
  sp_free(base_url);
  sp_free(user);
  sp_free((void*)account_path.data);
  sp_free((void*)payload.data);

  char* account_id = sp_str_to_cstr(client->account_id);
  sp_str_t index_payload = wxa_printf("[\"%s\"]", account_id);
  sp_str_t index_path = wxa_accounts_index_path(client);
  (void)wxa_write_text_file(index_path, index_payload);
  sp_free(account_id);
  sp_free((void*)index_path.data);
  sp_free((void*)index_payload.data);
}

static void wxa_save_sync_buf(wxa_client_t* client) {
  if (client->account_id.len == 0U || client->sync_buf.len == 0U) {
    return;
  }
  wxa_log_sync_state(client, "save", client->sync_buf);
  sp_str_t sync_path = wxa_sync_buf_path(client, client->account_id);
  char* sync_buf = sp_str_to_cstr(client->sync_buf);
  sp_str_t payload = wxa_printf(
    "{\"get_updates_buf\":\"%s\",\"last_seq\":%ld,\"last_message_id\":%ld}",
    sync_buf,
    client->last_seq,
    client->last_message_id
  );
  (void)wxa_write_text_file(sync_path, payload);
  sp_free(sync_buf);
  sp_free((void*)payload.data);
  sp_free((void*)sync_path.data);
}

static void wxa_clear_sync_buf(wxa_client_t* client) {
  if (client == NULL || client->account_id.len == 0U) {
    return;
  }
  sp_str_t sync_path = wxa_sync_buf_path(client, client->account_id);
  sp_str_t payload = wxa_printf(
    "{\"get_updates_buf\":\"\",\"last_seq\":%ld,\"last_message_id\":%ld}",
    client->last_seq,
    client->last_message_id
  );
  (void)wxa_write_text_file(sync_path, payload);
  wxa_free_str(&payload);
  wxa_free_str(&sync_path);
}

static bool wxa_pid_is_alive(pid_t pid) {
  if (pid <= 0) {
    return false;
  }
  return kill(pid, 0) == 0 || errno == EPERM;
}

static void wxa_release_monitor_lock(wxa_client_t* client) {
  if (client == NULL || client->monitor_lock_path.len == 0U) {
    return;
  }
  char* path = sp_str_to_cstr(client->monitor_lock_path);
  (void)remove(path);
  sp_free(path);
  wxa_free_str(&client->monitor_lock_path);
}

static wxa_status_t wxa_acquire_monitor_lock(wxa_client_t* client) {
  if (client == NULL || client->account_id.len == 0U) {
    return WXA_OK;
  }

  sp_str_t accounts_root = wxa_accounts_root(client);
  if (!wxa_ensure_dir(accounts_root)) {
    sp_free((void*)accounts_root.data);
    return wxa_fail(client, WXA_ERR_NETWORK, "failed to create accounts directory");
  }
  sp_free((void*)accounts_root.data);

  wxa_release_monitor_lock(client);

  for (int attempt = 0; attempt < 2; ++attempt) {
    sp_str_t lock_path = wxa_monitor_lock_path(client, client->account_id);
    char* lock_path_c = sp_str_to_cstr(lock_path);
    int fd = open(lock_path_c, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd >= 0) {
      char pid_buf[32];
      int pid_len = snprintf(pid_buf, sizeof(pid_buf), "%ld\n", (long)getpid());
      if (pid_len > 0) {
        ssize_t written = write(fd, pid_buf, (size_t)pid_len);
        (void)written;
      }
      (void)close(fd);
      client->monitor_lock_path = lock_path;
      sp_free(lock_path_c);
      return WXA_OK;
    }

    if (errno != EEXIST) {
      sp_free(lock_path_c);
      wxa_free_str(&lock_path);
      return wxa_fail(client, WXA_ERR_NETWORK, "failed to create monitor lock");
    }

    FILE* fp = fopen(lock_path_c, "rb");
    long existing_pid = 0L;
    if (fp != NULL) {
      int scan_rc = fscanf(fp, "%ld", &existing_pid);
      (void)scan_rc;
      fclose(fp);
    }

    if (wxa_pid_is_alive((pid_t)existing_pid)) {
      sp_free(lock_path_c);
      wxa_free_str(&lock_path);
      return wxa_fail(client, WXA_ERR_PROTOCOL, "monitor already running for account");
    }

    (void)remove(lock_path_c);
    sp_free(lock_path_c);
    wxa_free_str(&lock_path);
  }

  return wxa_fail(client, WXA_ERR_PROTOCOL, "failed to recover stale monitor lock");
}

static void wxa_try_load_persisted_account(wxa_client_t* client) {
  sp_str_t index_path = wxa_accounts_index_path(client);
  sp_str_t index_body = wxa_read_text_file(index_path);
  sp_free((void*)index_path.data);
  sp_str_t account_id = wxa_json_get_string(index_body.data, "");
  if (account_id.len == 0U) {
    char* first_quote = strchr(index_body.data != NULL ? index_body.data : "", '"');
    if (first_quote != NULL) {
      sp_str_t temp = sp_str_view(first_quote);
      wxa_free_str(&account_id);
      account_id = wxa_json_get_string(temp.data, "");
    }
  }
  if (account_id.len == 0U) {
    char* first = strchr(index_body.data != NULL ? index_body.data : "", '"');
    if (first != NULL) {
      char* second = strchr(first + 1, '"');
      if (second != NULL && second > first + 1) {
        account_id = sp_str_from_cstr_sized(first + 1, (u32)(second - first - 1));
      }
    }
  }
  wxa_free_str(&index_body);
  if (account_id.len == 0U) {
    return;
  }

  sp_str_t account_path = wxa_account_json_path(client, account_id);
  sp_str_t account_body = wxa_read_text_file(account_path);
  sp_free((void*)account_path.data);
  if (account_body.len == 0U) {
    wxa_free_str(&account_id);
    return;
  }

  sp_str_t token = wxa_json_get_string(account_body.data, "token");
  sp_str_t base_url = wxa_json_get_string(account_body.data, "baseUrl");
  if (client->bot_token.len == 0U && token.len > 0U) {
    client->bot_token = sp_str_copy(token);
  }
  if (client->account_id.len == 0U && account_id.len > 0U) {
    client->account_id = sp_str_copy(account_id);
  }
  if (base_url.len > 0U) {
    wxa_free_str(&client->base_url);
    client->base_url = sp_str_copy(base_url);
  }

  if (client->account_id.len > 0U) {
    sp_str_t sync_path = wxa_sync_buf_path(client, client->account_id);
    sp_str_t sync_body = wxa_read_text_file(sync_path);
    sp_free((void*)sync_path.data);
    if (sync_body.len > 0U) {
      sp_str_t sync = wxa_json_get_string(sync_body.data, "get_updates_buf");
      if (sync.len > 0U) {
        client->sync_buf = sp_str_copy(sync);
      }
      bool found_last_seq = false;
      bool found_last_message_id = false;
      long persisted_last_seq = wxa_json_get_top_level_long(sync_body, "last_seq", &found_last_seq);
      long persisted_last_message_id = wxa_json_get_top_level_long(sync_body, "last_message_id", &found_last_message_id);
      if (found_last_seq && found_last_message_id) {
        client->last_seq = persisted_last_seq;
        client->last_message_id = persisted_last_message_id;
      }
      wxa_free_str(&sync);
      wxa_free_str(&sync_body);
    }
  }

  wxa_free_str(&token);
  wxa_free_str(&base_url);
  wxa_free_str(&account_id);
  wxa_free_str(&account_body);
}

static sp_str_t wxa_hex_encode(const unsigned char* data, size_t len) {
  static const char* hex = "0123456789abcdef";
  sp_str_t out = sp_str_alloc((u32)(len * 2U));
  for (size_t i = 0; i < len; ++i) {
    ((char*)out.data)[i * 2U] = hex[(data[i] >> 4U) & 0x0F];
    ((char*)out.data)[i * 2U + 1U] = hex[data[i] & 0x0F];
  }
  out.len = (u32)(len * 2U);
  ((char*)out.data)[out.len] = '\0';
  return out;
}

static bool wxa_hex_decode(sp_str_t hex_text, unsigned char* out, size_t expected_len) {
  if (hex_text.len != expected_len * 2U) {
    return false;
  }
  for (size_t i = 0; i < expected_len; ++i) {
    char hi = (char)tolower((unsigned char)hex_text.data[i * 2U]);
    char lo = (char)tolower((unsigned char)hex_text.data[i * 2U + 1U]);
    unsigned char hv = (unsigned char)((hi >= 'a') ? (hi - 'a' + 10) : (hi - '0'));
    unsigned char lv = (unsigned char)((lo >= 'a') ? (lo - 'a' + 10) : (lo - '0'));
    out[i] = (unsigned char)((hv << 4U) | lv);
  }
  return true;
}

static sp_str_t wxa_base64_encode(const unsigned char* data, size_t len) {
  int out_len = 4 * ((int)((len + 2U) / 3U));
  unsigned char* out = sp_alloc((u32)out_len + 1U);
  EVP_EncodeBlock(out, data, (int)len);
  out[out_len] = '\0';
  return sp_str((const char*)out, (u32)out_len);
}

static bool wxa_base64_decode(sp_str_t text, unsigned char* out, size_t* out_len) {
  int len = EVP_DecodeBlock(out, (const unsigned char*)text.data, (int)text.len);
  if (len < 0) {
    return false;
  }
  while (text.len > 0U && text.data[text.len - 1U] == '=') {
    len--;
    text.len--;
  }
  *out_len = (size_t)len;
  return true;
}

static bool wxa_evp_crypt(
  const unsigned char* in,
  size_t in_len,
  const unsigned char key[16],
  bool encrypt,
  unsigned char** out,
  size_t* out_len
) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  int len1 = 0;
  int len2 = 0;
  unsigned char* buffer = NULL;
  if (ctx == NULL) {
    return false;
  }
  buffer = sp_alloc((u32)in_len + 32U);
  if (buffer == NULL) {
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  if (encrypt) {
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL) != 1) {
      EVP_CIPHER_CTX_free(ctx);
      sp_free(buffer);
      return false;
    }
    if (EVP_EncryptUpdate(ctx, buffer, &len1, in, (int)in_len) != 1 ||
        EVP_EncryptFinal_ex(ctx, buffer + len1, &len2) != 1) {
      EVP_CIPHER_CTX_free(ctx);
      sp_free(buffer);
      return false;
    }
  } else {
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL) != 1) {
      EVP_CIPHER_CTX_free(ctx);
      sp_free(buffer);
      return false;
    }
    if (EVP_DecryptUpdate(ctx, buffer, &len1, in, (int)in_len) != 1 ||
        EVP_DecryptFinal_ex(ctx, buffer + len1, &len2) != 1) {
      EVP_CIPHER_CTX_free(ctx);
      sp_free(buffer);
      return false;
    }
  }
  EVP_CIPHER_CTX_free(ctx);
  *out = buffer;
  *out_len = (size_t)(len1 + len2);
  return true;
}

static bool wxa_md5_file(const char* file_path, sp_str_t* out_hex, u64* out_size) {
  FILE* fp = fopen(file_path, "rb");
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  unsigned char buffer[8192];
  u64 total = 0U;
  if (fp == NULL || ctx == NULL) {
    if (fp != NULL) fclose(fp);
    if (ctx != NULL) EVP_MD_CTX_free(ctx);
    return false;
  }
  if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1) {
    fclose(fp);
    EVP_MD_CTX_free(ctx);
    return false;
  }
  for (;;) {
    size_t got = fread(buffer, 1U, sizeof(buffer), fp);
    if (got > 0U) {
      total += (u64)got;
      EVP_DigestUpdate(ctx, buffer, got);
    }
    if (got < sizeof(buffer)) {
      break;
    }
  }
  fclose(fp);
  if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
    EVP_MD_CTX_free(ctx);
    return false;
  }
  EVP_MD_CTX_free(ctx);
  *out_hex = wxa_hex_encode(digest, digest_len);
  *out_size = total;
  return true;
}

static sp_str_t wxa_guess_mime(sp_str_t path_or_name) {
  const char* ext = strrchr(path_or_name.data != NULL ? path_or_name.data : "", '.');
  if (ext == NULL) return sp_str_lit("application/octet-stream");
  if (strcasecmp(ext, ".png") == 0) return sp_str_lit("image/png");
  if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return sp_str_lit("image/jpeg");
  if (strcasecmp(ext, ".gif") == 0) return sp_str_lit("image/gif");
  if (strcasecmp(ext, ".webp") == 0) return sp_str_lit("image/webp");
  if (strcasecmp(ext, ".bmp") == 0) return sp_str_lit("image/bmp");
  if (strcasecmp(ext, ".mp4") == 0) return sp_str_lit("video/mp4");
  if (strcasecmp(ext, ".mov") == 0) return sp_str_lit("video/quicktime");
  if (strcasecmp(ext, ".wav") == 0) return sp_str_lit("audio/wav");
  if (strcasecmp(ext, ".mp3") == 0) return sp_str_lit("audio/mpeg");
  if (strcasecmp(ext, ".pdf") == 0) return sp_str_lit("application/pdf");
  if (strcasecmp(ext, ".txt") == 0) return sp_str_lit("text/plain");
  return sp_str_lit("application/octet-stream");
}

static sp_str_t wxa_guess_extension(sp_str_t mime) {
  if (sp_str_equal(mime, sp_str_lit("image/png"))) return sp_str_lit(".png");
  if (sp_str_equal(mime, sp_str_lit("image/jpeg"))) return sp_str_lit(".jpg");
  if (sp_str_equal(mime, sp_str_lit("image/gif"))) return sp_str_lit(".gif");
  if (sp_str_equal(mime, sp_str_lit("video/mp4"))) return sp_str_lit(".mp4");
  if (sp_str_equal(mime, sp_str_lit("audio/wav"))) return sp_str_lit(".wav");
  if (sp_str_equal(mime, sp_str_lit("application/pdf"))) return sp_str_lit(".pdf");
  return sp_str_lit(".bin");
}

static sp_str_t wxa_cdn_download_url(wxa_client_t* client, sp_str_t param) {
  CURL* curl = curl_easy_init();
  char* escaped = curl_easy_escape(curl, param.data, (int)param.len);
  sp_str_t url = sp_format("{}/download?encrypted_query_param={}", SP_FMT_STR(client->cdn_base_url), SP_FMT_CSTR(escaped));
  curl_free(escaped);
  curl_easy_cleanup(curl);
  return url;
}

static sp_str_t wxa_cdn_upload_url(wxa_client_t* client, sp_str_t upload_param, sp_str_t filekey) {
  CURL* curl = curl_easy_init();
  char* escaped_param = curl_easy_escape(curl, upload_param.data, (int)upload_param.len);
  char* escaped_key = curl_easy_escape(curl, filekey.data, (int)filekey.len);
  sp_str_t url = sp_format(
    "{}/upload?encrypted_query_param={}&filekey={}",
    SP_FMT_STR(client->cdn_base_url),
    SP_FMT_CSTR(escaped_param),
    SP_FMT_CSTR(escaped_key)
  );
  curl_free(escaped_param);
  curl_free(escaped_key);
  curl_easy_cleanup(curl);
  return url;
}

static size_t wxa_header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
  size_t total = size * nitems;
  wxa_http_response_t* response = (wxa_http_response_t*)userdata;
  if (response == NULL || total == 0U) {
    return total;
  }
  if (strncasecmp(buffer, "x-encrypted-param:", 18) == 0) {
    const char* value = buffer + 18;
    while (*value == ' ' || *value == '\t') value++;
    const char* end = value + strlen(value);
    while (end > value && (end[-1] == '\r' || end[-1] == '\n')) end--;
    response->encrypted_param = sp_str_from_cstr_sized(value, (u32)(end - value));
  } else if (strncasecmp(buffer, "x-error-message:", 16) == 0) {
    const char* value = buffer + 16;
    while (*value == ' ' || *value == '\t') value++;
    const char* end = value + strlen(value);
    while (end > value && (end[-1] == '\r' || end[-1] == '\n')) end--;
    response->error_message = sp_str_from_cstr_sized(value, (u32)(end - value));
  }
  return total;
}
static sp_str_t wxa_random_wechat_uin(void) {
  unsigned char seed[4];
  u32 value;
  char decimal[32];
  unsigned char encoded[64];

  if (RAND_bytes(seed, (int)sizeof(seed)) == 1) {
    value =
      ((u32)seed[0] << 24U) |
      ((u32)seed[1] << 16U) |
      ((u32)seed[2] << 8U) |
      (u32)seed[3];
  } else {
    value = (u32)time(NULL) ^ (u32)getpid();
  }

  (void)snprintf(decimal, sizeof(decimal), "%u", value);
  (void)EVP_EncodeBlock(encoded, (const unsigned char*)decimal, (int)strlen(decimal));
  return sp_str_from_cstr((const char*)encoded);
}

static wxa_status_t wxa_http_request(
  wxa_client_t* client,
  const char* method,
  sp_str_t url,
  const char* body,
  long timeout_ms,
  bool auth_required,
  wxa_http_response_t* out
) {
  wxa_status_t status = WXA_OK;
  char error_buf[CURL_ERROR_SIZE];
  if (!wxa_curl_init_once()) {
    return wxa_fail(client, WXA_ERR_NETWORK, "curl_global_init failed");
  }

  for (int attempt = 1; attempt <= WXA_CURL_RETRY_MAX; ++attempt) {
    CURL* curl = curl_easy_init();
    struct curl_slist* headers = NULL;
    wxa_buffer_t buffer = {0};
    CURLcode rc;
    char* c_url;

    if (out != NULL) {
      wxa_free_str(&out->body);
      wxa_free_str(&out->encrypted_param);
      wxa_free_str(&out->error_message);
      out->http_status = 0L;
    }
    error_buf[0] = '\0';

    if (curl == NULL) {
      return wxa_fail(client, WXA_ERR_NETWORK, "curl_easy_init failed");
    }

    c_url = sp_str_to_cstr(url);
    curl_easy_setopt(curl, CURLOPT_URL, c_url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, WXA_DEFAULT_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wxa_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, wxa_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "weixin-agent-c-sdk/0.1.0");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buf);

    if (body != NULL) {
      sp_str_t content_length = wxa_printf("Content-Length: %zu", strlen(body));
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
      headers = curl_slist_append(headers, "Content-Type: application/json");
      headers = curl_slist_append(headers, content_length.data);
      wxa_free_str(&content_length);
    } else if (strcmp(method, "GET") != 0) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    if (auth_required && client != NULL && client->bot_token.len > 0U) {
      sp_str_t auth = sp_format("Authorization: Bearer {}", SP_FMT_STR(client->bot_token));
      headers = curl_slist_append(headers, auth.data);
      wxa_free_str(&auth);
      headers = curl_slist_append(headers, "AuthorizationType: ilink_bot_token");
      sp_str_t wechat_uin = wxa_random_wechat_uin();
      sp_str_t uin_header = sp_format("X-WECHAT-UIN: {}", SP_FMT_STR(wechat_uin));
      headers = curl_slist_append(headers, uin_header.data);
      wxa_free_str(&uin_header);
      wxa_free_str(&wechat_uin);
    }

    if (headers != NULL) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->http_status);
      out->body = wxa_buffer_to_str(&buffer);
      if (out->http_status < 200L || out->http_status >= 300L) {
        sp_str_t msg = sp_format(
          "http {} failed with status {}",
          SP_FMT_CSTR(method),
          SP_FMT_S64((s64)out->http_status)
        );
        status = wxa_fail(client, WXA_ERR_NETWORK, msg.data);
        wxa_free_str(&msg);
      } else {
        status = WXA_OK;
      }
    } else {
      const char* msg = error_buf[0] != '\0' ? error_buf : curl_easy_strerror(rc);
      status = wxa_fail(client, WXA_ERR_NETWORK, msg);
    }

    if (headers != NULL) {
      curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
    sp_free(c_url);
    if (buffer.data != NULL) {
      sp_dyn_array_free(buffer.data);
    }

    if (status == WXA_OK) {
      return WXA_OK;
    }
    if (rc != CURLE_SSL_CONNECT_ERROR && rc != CURLE_OPERATION_TIMEDOUT && rc != CURLE_COULDNT_CONNECT) {
      return status;
    }
    if (attempt < WXA_CURL_RETRY_MAX) {
      wxa_sleep_ms((unsigned int)(250 * attempt));
    }
  }
  return status;
}

static wxa_status_t wxa_http_get(
  wxa_client_t* client,
  sp_str_t url,
  long timeout_ms,
  bool auth_required,
  wxa_http_response_t* out
) {
  return wxa_http_request(client, "GET", url, NULL, timeout_ms, auth_required, out);
}

static wxa_status_t wxa_http_post_json(
  wxa_client_t* client,
  sp_str_t url,
  const char* body,
  long timeout_ms,
  wxa_http_response_t* out
) {
  return wxa_http_request(client, "POST", url, body, timeout_ms, true, out);
}

static bool wxa_read_file_bytes(const char* file_path, unsigned char** out, size_t* out_len) {
  FILE* fp = fopen(file_path, "rb");
  long size;
  unsigned char* data;
  if (fp == NULL) {
    return false;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    fclose(fp);
    return false;
  }
  size = ftell(fp);
  if (size < 0L) {
    fclose(fp);
    return false;
  }
  rewind(fp);
  data = sp_alloc((u32)size + 1U);
  if (data == NULL) {
    fclose(fp);
    return false;
  }
  *out_len = fread(data, 1U, (size_t)size, fp);
  fclose(fp);
  *out = data;
  return true;
}

static bool wxa_save_buffer_file(sp_str_t file_path, const unsigned char* data, size_t len) {
  char* path = sp_str_to_cstr(file_path);
  FILE* fp = fopen(path, "wb");
  if (fp == NULL) {
    sp_free(path);
    return false;
  }
  bool ok = fwrite(data, 1U, len, fp) == len;
  fclose(fp);
  sp_free(path);
  return ok;
}

static sp_str_t wxa_temp_media_path(wxa_client_t* client, sp_str_t mime, const char* prefix) {
  sp_str_t ext = wxa_guess_extension(mime);
  sp_str_t inbound = wxa_join_path2(client->media_dir, "inbound");
  (void)wxa_ensure_dir(inbound);
  sp_str_t path = sp_format("{}/{}-{}{}", SP_FMT_STR(inbound), SP_FMT_CSTR(prefix), SP_FMT_U64((u64)time(NULL)), SP_FMT_STR(ext));
  sp_free((void*)inbound.data);
  return path;
}

static wxa_status_t wxa_download_media(
  wxa_client_t* client,
  wxa_media_type_t media_type,
  sp_str_t encrypt_param,
  sp_str_t aes_key_base64,
  sp_str_t hex_aes_key,
  sp_str_t file_name,
  wxa_media_t* out
) {
  unsigned char key[32];
  size_t key_len = 0U;
  unsigned char* encrypted = NULL;
  size_t encrypted_len = 0U;
  unsigned char* decrypted = NULL;
  size_t decrypted_len = 0U;
  sp_str_t url = wxa_cdn_download_url(client, encrypt_param);
  wxa_http_response_t response = {0};
  wxa_status_t status = wxa_http_get(client, url, WXA_DEFAULT_API_TIMEOUT_MS, false, &response);
  sp_free((void*)url.data);
  if (status != WXA_OK) {
    return status;
  }
  encrypted = (unsigned char*)response.body.data;
  encrypted_len = response.body.len;
  if (aes_key_base64.len > 0U) {
    if (!wxa_base64_decode(aes_key_base64, key, &key_len)) {
      return wxa_fail(client, WXA_ERR_PROTOCOL, "invalid media aes_key");
    }
    if (key_len == 32U) {
      sp_str_t hex_text = sp_str((const char*)key, (u32)key_len);
      if (!wxa_hex_decode(hex_text, key, 16U)) {
        return wxa_fail(client, WXA_ERR_PROTOCOL, "invalid media hex aes_key");
      }
      key_len = 16U;
    }
    if (!wxa_evp_crypt(encrypted, encrypted_len, key, false, &decrypted, &decrypted_len)) {
      return wxa_fail(client, WXA_ERR_PROTOCOL, "media decrypt failed");
    }
  } else if (hex_aes_key.len > 0U) {
    if (!wxa_hex_decode(hex_aes_key, key, 16U)) {
      return wxa_fail(client, WXA_ERR_PROTOCOL, "invalid image hex aeskey");
    }
    if (!wxa_evp_crypt(encrypted, encrypted_len, key, false, &decrypted, &decrypted_len)) {
      return wxa_fail(client, WXA_ERR_PROTOCOL, "media decrypt failed");
    }
  } else {
    decrypted = sp_alloc((u32)encrypted_len + 1U);
    memcpy(decrypted, encrypted, encrypted_len);
    decrypted_len = encrypted_len;
  }

  sp_str_t mime = media_type == WXA_MEDIA_IMAGE
    ? sp_str_lit("image/jpeg")
    : media_type == WXA_MEDIA_VIDEO
      ? sp_str_lit("video/mp4")
      : media_type == WXA_MEDIA_AUDIO
        ? sp_str_lit("audio/silk")
        : (file_name.len > 0U ? wxa_guess_mime(file_name) : sp_str_lit("application/octet-stream"));
  sp_str_t save_path = wxa_temp_media_path(client, mime, "media");
  if (!wxa_save_buffer_file(save_path, decrypted, decrypted_len)) {
    sp_free(decrypted);
    return wxa_fail(client, WXA_ERR_NETWORK, "failed to persist media file");
  }

  out->type = media_type;
  out->file_path = save_path.data;
  out->mime_type = mime.data;
  out->file_name = file_name.len > 0U ? file_name.data : NULL;
  sp_free(decrypted);
  return WXA_OK;
}

static wxa_status_t wxa_cdn_upload(
  wxa_client_t* client,
  sp_str_t upload_param,
  sp_str_t filekey,
  const unsigned char* ciphertext,
  size_t ciphertext_len,
  sp_str_t* out_download_param
) {
  CURL* curl = curl_easy_init();
  struct curl_slist* headers = NULL;
  wxa_http_response_t response = {0};
  CURLcode rc;
  sp_str_t url = wxa_cdn_upload_url(client, upload_param, filekey);
  if (curl == NULL) {
    sp_free((void*)url.data);
    return wxa_fail(client, WXA_ERR_NETWORK, "curl init failed for cdn upload");
  }
  curl_easy_setopt(curl, CURLOPT_URL, sp_str_to_cstr(url));
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ciphertext);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)ciphertext_len);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, WXA_DEFAULT_API_TIMEOUT_MS);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wxa_write_cb);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, wxa_header_cb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
  headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.http_status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  sp_free((void*)url.data);
  if (rc != CURLE_OK) {
    return wxa_fail(client, WXA_ERR_NETWORK, curl_easy_strerror(rc));
  }
  if (response.http_status != 200L || response.encrypted_param.len == 0U) {
    return wxa_fail(client, WXA_ERR_NETWORK, "cdn upload failed");
  }
  *out_download_param = sp_str_copy(response.encrypted_param);
  wxa_free_str(&response.encrypted_param);
  wxa_free_str(&response.error_message);
  return WXA_OK;
}

static wxa_status_t wxa_get_upload_url(
  wxa_client_t* client,
  sp_str_t filekey,
  int media_type,
  const char* to_user_id,
  u64 raw_size,
  sp_str_t raw_md5,
  u64 cipher_size,
  sp_str_t aeskey_hex,
  sp_str_t* out_upload_param
) {
  sp_str_t escaped_to = wxa_json_escape(to_user_id);
  char* filekey_c = sp_str_to_cstr(filekey);
  char* escaped_to_c = sp_str_to_cstr(escaped_to);
  char* raw_md5_c = sp_str_to_cstr(raw_md5);
  char* aeskey_hex_c = sp_str_to_cstr(aeskey_hex);
  sp_str_t body = wxa_printf(
    "{\"filekey\":\"%s\",\"media_type\":%d,\"to_user_id\":\"%s\",\"rawsize\":%llu,"
    "\"rawfilemd5\":\"%s\",\"filesize\":%llu,\"no_need_thumb\":true,\"aeskey\":\"%s\","
    "\"base_info\":{\"channel_version\":\"0.1.0\"}}",
    filekey_c,
    media_type,
    escaped_to_c,
    (unsigned long long)raw_size,
    raw_md5_c,
    (unsigned long long)cipher_size,
    aeskey_hex_c
  );
  sp_str_t url = wxa_build_url(client->base_url, "ilink/bot/getuploadurl");
  wxa_http_response_t response = {0};
  wxa_status_t status = wxa_http_post_json(client, url, body.data, WXA_DEFAULT_API_TIMEOUT_MS, &response);
  if (status == WXA_OK) {
    *out_upload_param = wxa_json_get_string(response.body.data, "upload_param");
    if (out_upload_param->len == 0U) {
      status = wxa_fail(client, WXA_ERR_PROTOCOL, "missing upload_param");
    }
  }
  wxa_free_str(&response.body);
  wxa_free_str(&url);
  wxa_free_str(&body);
  wxa_free_str(&escaped_to);
  sp_free(filekey_c);
  sp_free(escaped_to_c);
  sp_free(raw_md5_c);
  sp_free(aeskey_hex_c);
  return status;
}

static wxa_status_t wxa_upload_media(
  wxa_client_t* client,
  const wxa_media_t* media,
  const char* to_user_id,
  wxa_uploaded_media_t* out
) {
  unsigned char* plaintext = NULL;
  size_t plaintext_len = 0U;
  unsigned char aeskey[16];
  unsigned char* ciphertext = NULL;
  size_t ciphertext_len = 0U;
  int upload_media_type = media->type == WXA_MEDIA_VIDEO ? 2 : media->type == WXA_MEDIA_FILE ? 3 : media->type == WXA_MEDIA_AUDIO ? 4 : 1;
  if (media->file_path == NULL) {
    return wxa_fail(client, WXA_ERR_ARGUMENT, "media.file_path is required");
  }
  if (!wxa_read_file_bytes(media->file_path, &plaintext, &plaintext_len)) {
    return wxa_fail(client, WXA_ERR_ARGUMENT, "failed to read outbound media file");
  }
  if (RAND_bytes(aeskey, sizeof(aeskey)) != 1) {
    sp_free(plaintext);
    return wxa_fail(client, WXA_ERR_PROTOCOL, "RAND_bytes failed");
  }
  if (!wxa_evp_crypt(plaintext, plaintext_len, aeskey, true, &ciphertext, &ciphertext_len)) {
    sp_free(plaintext);
    return wxa_fail(client, WXA_ERR_PROTOCOL, "media encrypt failed");
  }

  sp_str_t md5_hex = sp_str_lit("");
  u64 raw_size = 0U;
  if (!wxa_md5_file(media->file_path, &md5_hex, &raw_size)) {
    sp_free(plaintext);
    sp_free(ciphertext);
    return wxa_fail(client, WXA_ERR_PROTOCOL, "failed to hash outbound media");
  }
  sp_str_t filekey = wxa_hex_encode(aeskey, 16U);
  sp_str_t aeskey_hex = wxa_hex_encode(aeskey, 16U);
  sp_str_t upload_param = sp_str_lit("");
  wxa_status_t status = wxa_get_upload_url(
    client,
    filekey,
    upload_media_type,
    to_user_id,
    raw_size,
    md5_hex,
    (u64)ciphertext_len,
    aeskey_hex,
    &upload_param
  );
  if (status == WXA_OK) {
    status = wxa_cdn_upload(client, upload_param, filekey, ciphertext, ciphertext_len, &out->download_param);
  }
  if (status == WXA_OK) {
    out->aeskey_hex = sp_str_copy(aeskey_hex);
    out->file_size_plain = raw_size;
    out->file_size_cipher = (u64)ciphertext_len;
  }
  wxa_free_str(&md5_hex);
  wxa_free_str(&filekey);
  wxa_free_str(&aeskey_hex);
  wxa_free_str(&upload_param);
  sp_free(plaintext);
  sp_free(ciphertext);
  return status;
}

static wxa_send_result_t wxa_sendmessage_post_raw(
  wxa_client_t* client,
  sp_str_t* body,
  long* out_ret,
  long* out_errcode
) {
  if (body == NULL || body->data == NULL || body->len == 0U) {
    return WXA_SEND_RESULT_FATAL;
  }
  unsigned long long now = wxa_now_millis();
  if (client->last_send_at_ms > 0ULL && now > client->last_send_at_ms) {
    unsigned long long elapsed = now - client->last_send_at_ms;
    if (elapsed < WXA_SEND_MIN_INTERVAL_MS) {
      wxa_sleep_ms((unsigned int)(WXA_SEND_MIN_INTERVAL_MS - elapsed));
    }
  }
  (void)wxa_refresh_client_id_in_send_body(body);
  sp_str_t url = wxa_build_url(client->base_url, "ilink/bot/sendmessage");
  wxa_http_response_t response = {0};
  wxa_status_t status = wxa_http_post_json(client, url, body->data, WXA_SEND_API_TIMEOUT_MS, &response);
  client->last_send_at_ms = wxa_now_millis();
  bool found_ret = false;
  bool found_errcode = false;
  long ret = status == WXA_OK ? wxa_json_get_top_level_long(response.body, "ret", &found_ret) : 0L;
  long errcode = status == WXA_OK ? wxa_json_get_top_level_long(response.body, "errcode", &found_errcode) : 0L;
  if (out_ret != NULL) {
    *out_ret = found_ret ? ret : 0L;
  }
  if (out_errcode != NULL) {
    *out_errcode = found_errcode ? errcode : 0L;
  }

  if (status == WXA_OK) {
    if (found_ret && ret == -1L && (!found_errcode || errcode == 0L)) {
      return WXA_SEND_RESULT_RETRYABLE;
    }
    if (((found_ret && ret != 0L) || (found_errcode && errcode != 0L)) &&
        !(found_ret && ret == -1L && (!found_errcode || errcode == 0L))) {
      sp_str_t msg = sp_format(
        "sendmessage http-ok with api ret={} errcode={} body={}",
        SP_FMT_S64((s64)(found_ret ? ret : 0L)),
        SP_FMT_S64((s64)(found_errcode ? errcode : 0L)),
        SP_FMT_STR(response.body)
      );
      wxa_log(client, msg.data);
      wxa_free_str(&msg);
    }
    wxa_free_str(&response.body);
    wxa_free_str(&url);
    return WXA_SEND_RESULT_OK;
  }

  wxa_free_str(&response.body);
  wxa_free_str(&url);
  if (status == WXA_ERR_NETWORK || status == WXA_ERR_TIMEOUT) {
    return WXA_SEND_RESULT_RETRYABLE;
  }
  return WXA_SEND_RESULT_FATAL;
}

static void wxa_retry_send_remove_at(wxa_client_t* client, u32 index) {
  u32 size = sp_dyn_array_size(client->retry_sends);
  if (size == 0U || index >= size) {
    return;
  }
  wxa_free_str(&client->retry_sends[index].body);
  if (index != size - 1U) {
    client->retry_sends[index] = client->retry_sends[size - 1U];
  }
  sp_dyn_array_pop(client->retry_sends);
}

static bool wxa_enqueue_retry_send(wxa_client_t* client, sp_str_t body) {
  if (client == NULL || body.data == NULL || body.len == 0U) {
    return false;
  }
  if (sp_dyn_array_size(client->retry_sends) >= WXA_RETRY_QUEUE_MAX) {
    wxa_log(client, "retry queue full, drop oldest outbound");
    wxa_retry_send_remove_at(client, 0U);
  }
  wxa_retry_send_t entry = {
    .body = sp_str_copy(body),
    .attempts = 0U,
    .next_retry_at_ms = wxa_now_millis() + (unsigned long long)wxa_random_range_ms(40U, 100U)
  };
  sp_dyn_array_push(client->retry_sends, entry);
  return true;
}

static void wxa_process_retry_sends(wxa_client_t* client, unsigned int budget) {
  if (client == NULL || budget == 0U || sp_dyn_array_size(client->retry_sends) == 0U) {
    return;
  }

  unsigned int processed = 0U;
  u32 i = 0U;
  while (i < sp_dyn_array_size(client->retry_sends) && processed < budget) {
    unsigned long long now = wxa_now_millis();
    wxa_retry_send_t* entry = &client->retry_sends[i];
    if (entry->next_retry_at_ms > now) {
      i++;
      continue;
    }

    long ret = 0L;
    long errcode = 0L;
    wxa_send_result_t result = wxa_sendmessage_post_raw(client, &entry->body, &ret, &errcode);
    entry->attempts++;
    processed++;

    if (result == WXA_SEND_RESULT_OK) {
      sp_str_t msg = sp_format("retry send delivered attempts={}", SP_FMT_U32(entry->attempts));
      wxa_log(client, msg.data);
      wxa_free_str(&msg);
      wxa_retry_send_remove_at(client, i);
      continue;
    }

    if (result == WXA_SEND_RESULT_RETRYABLE && entry->attempts < WXA_RETRY_QUEUE_MAX_ATTEMPTS) {
      entry->next_retry_at_ms = now + (unsigned long long)wxa_retry_queue_delay_ms(entry->attempts);
      i++;
      continue;
    }

    sp_str_t msg = sp_format(
      "drop retry send attempts={} ret={} errcode={}",
      SP_FMT_U32(entry->attempts),
      SP_FMT_S64((s64)ret),
      SP_FMT_S64((s64)errcode)
    );
    wxa_log(client, msg.data);
    wxa_free_str(&msg);
    wxa_retry_send_remove_at(client, i);
  }
}

static sp_str_t wxa_build_send_text_body(const char* to_user_id, const char* context_token, const char* text) {
  sp_str_t escaped_text = wxa_json_escape(text != NULL ? text : "");
  sp_str_t escaped_to = wxa_json_escape(to_user_id != NULL ? to_user_id : "");
  sp_str_t escaped_context = wxa_json_escape(context_token != NULL ? context_token : "");
  unsigned long long client_ms = wxa_now_millis();
  unsigned long long client_nonce = wxa_next_client_nonce();
  char* escaped_text_c = sp_str_to_cstr(escaped_text);
  char* escaped_to_c = sp_str_to_cstr(escaped_to);
  char* escaped_context_c = sp_str_to_cstr(escaped_context);
  sp_str_t body = wxa_printf(
    "{\"msg\":{\"from_user_id\":\"\",\"to_user_id\":\"%s\",\"client_id\":\"c-%llu-%llu\","
    "\"message_type\":2,\"message_state\":2,"
    "\"item_list\":[{\"type\":1,\"text_item\":{\"text\":\"%s\"}}],"
    "\"context_token\":\"%s\"},\"base_info\":{\"channel_version\":\"0.1.0\"}}",
    escaped_to_c,
    client_ms,
    client_nonce,
    escaped_text_c,
    escaped_context_c
  );
  wxa_free_str(&escaped_text);
  wxa_free_str(&escaped_to);
  wxa_free_str(&escaped_context);
  sp_free(escaped_text_c);
  sp_free(escaped_to_c);
  sp_free(escaped_context_c);
  return body;
}

static wxa_status_t wxa_send_text(
  wxa_client_t* client,
  const char* to_user_id,
  const char* context_token,
  const char* text
) {
  sp_str_t body = wxa_build_send_text_body(to_user_id, context_token, text);
  wxa_status_t status = WXA_OK;
  wxa_send_result_t result = WXA_SEND_RESULT_RETRYABLE;
  long last_ret = 0L;
  long last_errcode = 0L;
  unsigned int attempts = 0U;
  for (attempts = 1U; attempts <= WXA_SEND_MAX_ATTEMPTS; ++attempts) {
    result = wxa_sendmessage_post_raw(client, &body, &last_ret, &last_errcode);
    if (result == WXA_SEND_RESULT_OK) {
      status = WXA_OK;
      break;
    }
    if (result == WXA_SEND_RESULT_FATAL) {
      sp_str_t fail_msg = sp_format(
        "sendmessage fatal ret={} errcode={}",
        SP_FMT_S64((s64)last_ret),
        SP_FMT_S64((s64)last_errcode)
      );
      status = wxa_fail(client, WXA_ERR_PROTOCOL, fail_msg.data);
      wxa_free_str(&fail_msg);
      break;
    }
    if (attempts < WXA_SEND_MAX_ATTEMPTS) {
      wxa_sleep_ms(wxa_send_retry_delay_ms(attempts));
    }
  }

  if (result == WXA_SEND_RESULT_RETRYABLE) {
    bool queued = false;
    if (context_token != NULL && context_token[0] != '\0') {
      sp_str_t fallback = wxa_build_send_text_body(to_user_id, "", text);
      long fb_ret = 0L;
      long fb_errcode = 0L;
      wxa_send_result_t fb_result = WXA_SEND_RESULT_RETRYABLE;
      for (unsigned int i = 0U; i < 2U; ++i) {
        fb_result = wxa_sendmessage_post_raw(client, &fallback, &fb_ret, &fb_errcode);
        if (fb_result == WXA_SEND_RESULT_OK) {
          wxa_log(client, "sendmessage fallback with empty context delivered");
          status = WXA_OK;
          break;
        }
        if (fb_result == WXA_SEND_RESULT_FATAL) {
          break;
        }
      }
      if (status != WXA_OK) {
        queued = wxa_enqueue_retry_send(client, fallback);
      }
      wxa_free_str(&fallback);
    } else {
      queued = wxa_enqueue_retry_send(client, body);
    }

    if (status == WXA_OK) {
      // no-op
    } else if (queued) {
      wxa_log(client, "sendmessage retry exhausted, queued for background delivery");
      status = WXA_OK;
    } else {
      sp_str_t fail_msg = sp_format(
        "sendmessage retry exhausted and enqueue failed ret={} errcode={}",
        SP_FMT_S64((s64)last_ret),
        SP_FMT_S64((s64)last_errcode)
      );
      status = wxa_fail(client, WXA_ERR_NETWORK, fail_msg.data);
      wxa_free_str(&fail_msg);
    }
  }

  wxa_free_str(&body);
  return status;
}

static sp_str_t wxa_get_typing_ticket(
  wxa_client_t* client,
  const char* ilink_user_id,
  const char* context_token
) {
  sp_str_t escaped_user = wxa_json_escape(ilink_user_id != NULL ? ilink_user_id : "");
  sp_str_t escaped_context = wxa_json_escape(context_token != NULL ? context_token : "");
  char* escaped_user_c = sp_str_to_cstr(escaped_user);
  char* escaped_context_c = sp_str_to_cstr(escaped_context);
  sp_str_t body = wxa_printf(
    "{\"ilink_user_id\":\"%s\",\"context_token\":\"%s\",\"base_info\":{\"channel_version\":\"0.1.0\"}}",
    escaped_user_c,
    escaped_context_c
  );
  sp_str_t url = wxa_build_url(client->base_url, "ilink/bot/getconfig");
  wxa_http_response_t response = {0};
  sp_str_t ticket = sp_str_lit("");
  if (wxa_http_post_json(client, url, body.data, WXA_DEFAULT_CONFIG_TIMEOUT_MS, &response) == WXA_OK) {
    ticket = wxa_json_get_string(response.body.data, "typing_ticket");
  }
  wxa_free_str(&response.body);
  wxa_free_str(&url);
  wxa_free_str(&body);
  wxa_free_str(&escaped_user);
  wxa_free_str(&escaped_context);
  sp_free(escaped_user_c);
  sp_free(escaped_context_c);
  return ticket;
}

static void wxa_send_typing(
  wxa_client_t* client,
  const char* ilink_user_id,
  sp_str_t typing_ticket,
  int status_code
) {
  if (typing_ticket.len == 0U) {
    return;
  }
  sp_str_t escaped_user = wxa_json_escape(ilink_user_id != NULL ? ilink_user_id : "");
  char* escaped_user_c = sp_str_to_cstr(escaped_user);
  char* typing_ticket_c = sp_str_to_cstr(typing_ticket);
  sp_str_t body = wxa_printf(
    "{\"ilink_user_id\":\"%s\",\"typing_ticket\":\"%s\",\"status\":%d,\"base_info\":{\"channel_version\":\"0.1.0\"}}",
    escaped_user_c,
    typing_ticket_c,
    status_code
  );
  sp_str_t url = wxa_build_url(client->base_url, "ilink/bot/sendtyping");
  wxa_http_response_t response = {0};
  (void)wxa_http_post_json(client, url, body.data, WXA_DEFAULT_CONFIG_TIMEOUT_MS, &response);
  wxa_free_str(&response.body);
  wxa_free_str(&url);
  wxa_free_str(&body);
  wxa_free_str(&escaped_user);
  sp_free(escaped_user_c);
  sp_free(typing_ticket_c);
}

static wxa_status_t wxa_send_uploaded_media(
  wxa_client_t* client,
  const char* to_user_id,
  const char* context_token,
  const wxa_media_t* media,
  const wxa_uploaded_media_t* uploaded,
  const char* text
) {
  sp_str_t escaped_to = wxa_json_escape(to_user_id);
  sp_str_t escaped_context = wxa_json_escape(context_token);
  sp_str_t aeskey_b64 = wxa_base64_encode((const unsigned char*)uploaded->aeskey_hex.data, uploaded->aeskey_hex.len);
  sp_str_t body = sp_str_lit("");
  char* escaped_to_c = sp_str_to_cstr(escaped_to);
  char* escaped_context_c = sp_str_to_cstr(escaped_context);
  char* download_param_c = sp_str_to_cstr(uploaded->download_param);
  char* aeskey_b64_c = sp_str_to_cstr(aeskey_b64);
  unsigned long long client_ms = wxa_now_millis();
  unsigned long long client_nonce = wxa_next_client_nonce();
  if (media->type == WXA_MEDIA_IMAGE) {
    body = wxa_printf(
      "{\"msg\":{\"from_user_id\":\"\",\"to_user_id\":\"%s\",\"client_id\":\"c-%llu-%llu\","
      "\"message_type\":2,\"message_state\":2,"
      "\"item_list\":[{\"type\":2,\"image_item\":{\"media\":{\"encrypt_query_param\":\"%s\",\"aes_key\":\"%s\",\"encrypt_type\":1},\"mid_size\":%llu}}],"
      "\"context_token\":\"%s\"},\"base_info\":{\"channel_version\":\"0.1.0\"}}",
      escaped_to_c,
      client_ms,
      client_nonce,
      download_param_c,
      aeskey_b64_c,
      (unsigned long long)uploaded->file_size_cipher,
      escaped_context_c
    );
  } else if (media->type == WXA_MEDIA_VIDEO) {
    body = wxa_printf(
      "{\"msg\":{\"from_user_id\":\"\",\"to_user_id\":\"%s\",\"client_id\":\"c-%llu-%llu\","
      "\"message_type\":2,\"message_state\":2,"
      "\"item_list\":[{\"type\":5,\"video_item\":{\"media\":{\"encrypt_query_param\":\"%s\",\"aes_key\":\"%s\",\"encrypt_type\":1},\"video_size\":%llu}}],"
      "\"context_token\":\"%s\"},\"base_info\":{\"channel_version\":\"0.1.0\"}}",
      escaped_to_c,
      client_ms,
      client_nonce,
      download_param_c,
      aeskey_b64_c,
      (unsigned long long)uploaded->file_size_cipher,
      escaped_context_c
    );
  } else {
    sp_str_t file_name = wxa_json_escape(media->file_name != NULL ? media->file_name : media->file_path);
    char* file_name_c = sp_str_to_cstr(file_name);
    body = wxa_printf(
      "{\"msg\":{\"from_user_id\":\"\",\"to_user_id\":\"%s\",\"client_id\":\"c-%llu-%llu\","
      "\"message_type\":2,\"message_state\":2,"
      "\"item_list\":[{\"type\":4,\"file_item\":{\"media\":{\"encrypt_query_param\":\"%s\",\"aes_key\":\"%s\",\"encrypt_type\":1},\"file_name\":\"%s\",\"len\":\"%llu\"}}],"
      "\"context_token\":\"%s\"},\"base_info\":{\"channel_version\":\"0.1.0\"}}",
      escaped_to_c,
      client_ms,
      client_nonce,
      download_param_c,
      aeskey_b64_c,
      file_name_c,
      (unsigned long long)uploaded->file_size_plain,
      escaped_context_c
    );
    sp_free(file_name_c);
    wxa_free_str(&file_name);
  }

  wxa_status_t status = WXA_OK;
  wxa_send_result_t result = WXA_SEND_RESULT_RETRYABLE;
  long last_ret = 0L;
  long last_errcode = 0L;
  unsigned int attempts = 0U;
  for (attempts = 1U; attempts <= WXA_SEND_MAX_ATTEMPTS; ++attempts) {
    result = wxa_sendmessage_post_raw(client, &body, &last_ret, &last_errcode);
    if (result == WXA_SEND_RESULT_OK) {
      status = WXA_OK;
      break;
    }
    if (result == WXA_SEND_RESULT_FATAL) {
      sp_str_t fail_msg = sp_format(
        "sendmedia fatal ret={} errcode={}",
        SP_FMT_S64((s64)last_ret),
        SP_FMT_S64((s64)last_errcode)
      );
      status = wxa_fail(client, WXA_ERR_PROTOCOL, fail_msg.data);
      wxa_free_str(&fail_msg);
      break;
    }
    if (attempts < WXA_SEND_MAX_ATTEMPTS) {
      wxa_sleep_ms(wxa_send_retry_delay_ms(attempts));
    }
  }

  if (result == WXA_SEND_RESULT_RETRYABLE) {
    if (wxa_enqueue_retry_send(client, body)) {
      wxa_log(client, "sendmedia retry exhausted, queued for background delivery");
      status = WXA_OK;
    } else {
      sp_str_t fail_msg = sp_format(
        "sendmedia retry exhausted and enqueue failed ret={} errcode={}",
        SP_FMT_S64((s64)last_ret),
        SP_FMT_S64((s64)last_errcode)
      );
      status = wxa_fail(client, WXA_ERR_NETWORK, fail_msg.data);
      wxa_free_str(&fail_msg);
    }
  }

  wxa_free_str(&body);
  wxa_free_str(&escaped_to);
  wxa_free_str(&escaped_context);
  wxa_free_str(&aeskey_b64);
  sp_free(escaped_to_c);
  sp_free(escaped_context_c);
  sp_free(download_param_c);
  sp_free(aeskey_b64_c);

  if (status == WXA_OK && text != NULL && text[0] != '\0') {
    status = wxa_send_text(client, to_user_id, context_token, text);
  }
  return status;
}

const char* wxa_version(void) {
  return "0.1.0-c";
}

wxa_client_t* wxa_client_new(const wxa_client_options_t* options) {
  wxa_client_t* client = sp_alloc_type(wxa_client_t);
  if (client == NULL) {
    return NULL;
  }

  client->base_url = wxa_copy_or_empty(options != NULL && options->base_url != NULL ? options->base_url : WXA_DEFAULT_BASE_URL);
  client->cdn_base_url = sp_str_from_cstr(WXA_DEFAULT_CDN_BASE_URL);
  client->bot_token = wxa_copy_or_empty(options != NULL ? options->bot_token : NULL);
  client->account_id = wxa_copy_or_empty(options != NULL ? options->account_id : NULL);
  client->sync_buf = sp_str_lit("");
  client->last_error = sp_str_lit("");
  client->state_dir = wxa_resolve_state_dir();
  client->media_dir = wxa_join_path2(client->state_dir, "weixin-agent-c-sdk-media");
  client->monitor_lock_path = sp_str_lit("");
  client->log_fn = options != NULL ? options->log_fn : NULL;
  client->log_user_data = options != NULL ? options->log_user_data : NULL;
  client->stop_requested = false;
  client->last_seq = 0L;
  client->last_message_id = 0L;
  client->retry_sends = NULL;
  client->last_send_at_ms = 0ULL;
  (void)wxa_ensure_dir(client->media_dir);
  wxa_try_load_persisted_account(client);
  return client;
}

void wxa_client_free(wxa_client_t* client) {
  if (client == NULL) {
    return;
  }
  sp_dyn_array_for(client->retry_sends, i) {
    wxa_free_str(&client->retry_sends[i].body);
  }
  sp_dyn_array_free(client->retry_sends);
  wxa_free_str(&client->base_url);
  wxa_free_str(&client->bot_token);
  wxa_free_str(&client->cdn_base_url);
  wxa_free_str(&client->account_id);
  wxa_free_str(&client->sync_buf);
  wxa_free_str(&client->last_error);
  wxa_free_str(&client->state_dir);
  wxa_free_str(&client->media_dir);
  wxa_release_monitor_lock(client);
  sp_free(client);
}

static void wxa_fill_login_result(
  wxa_login_result_t* result,
  sp_str_t account_id,
  sp_str_t user_id,
  sp_str_t bot_token,
  sp_str_t qrcode_url,
  sp_str_t session_qrcode,
  sp_str_t base_url
) {
  if (result == NULL) {
    return;
  }
  result->account_id = account_id.data;
  result->user_id = user_id.data;
  result->bot_token = bot_token.data;
  result->qrcode_url = qrcode_url.data;
  result->session_qrcode = session_qrcode.data;
  result->base_url = base_url.data;
}

wxa_status_t wxa_client_login(
  wxa_client_t* client,
  const wxa_login_options_t* options,
  wxa_login_result_t* result
) {
  if (client == NULL || result == NULL) {
    return wxa_fail(client, WXA_ERR_ARGUMENT, "client and result are required");
  }

  sp_str_t base_url = wxa_copy_or_empty(
    options != NULL && options->base_url != NULL ? options->base_url : client->base_url.data
  );
  sp_str_t bot_type = wxa_copy_or_empty(
    options != NULL && options->bot_type != NULL ? options->bot_type : WXA_DEFAULT_BOT_TYPE
  );
  unsigned int timeout_ms = options != NULL && options->timeout_ms > 0U
    ? options->timeout_ms
    : WXA_DEFAULT_LOGIN_TIMEOUT_MS;

  sp_str_t qr_url = wxa_build_url(base_url, "ilink/bot/get_bot_qrcode?bot_type=3");
  if (!sp_str_equal(bot_type, sp_str_lit("3"))) {
    sp_free((void*)qr_url.data);
    qr_url = sp_format("{}/ilink/bot/get_bot_qrcode?bot_type={}", SP_FMT_STR(base_url), SP_FMT_STR(bot_type));
  }

  wxa_http_response_t qr_response = {0};
  wxa_status_t status = wxa_http_get(client, qr_url, WXA_DEFAULT_API_TIMEOUT_MS, false, &qr_response);
  if (status != WXA_OK) {
    wxa_free_str(&base_url);
    wxa_free_str(&bot_type);
    wxa_free_str(&qr_url);
    return status;
  }

  sp_str_t qrcode = wxa_json_get_string(qr_response.body.data, "qrcode");
  sp_str_t qrcode_url = wxa_json_get_string(qr_response.body.data, "qrcode_img_content");
  sp_str_t scan_log = sp_format("scan this QR URL with WeChat: {}", SP_FMT_STR(qrcode_url));
  wxa_log(client, scan_log.data);
  wxa_free_str(&scan_log);

  long deadline = (long)time(NULL) + (long)(timeout_ms / 1000U);
  while ((long)time(NULL) < deadline) {
    sp_str_t encoded_qrcode = wxa_json_escape(qrcode.data);
    sp_str_t status_url = sp_format(
      "{}/ilink/bot/get_qrcode_status?qrcode={}",
      SP_FMT_STR(base_url),
      SP_FMT_STR(encoded_qrcode)
    );
    wxa_http_response_t poll_response = {0};
    status = wxa_http_get(client, status_url, (long)WXA_DEFAULT_LONG_POLL_TIMEOUT_MS, false, &poll_response);
    wxa_free_str(&encoded_qrcode);
    wxa_free_str(&status_url);
    if (status != WXA_OK) {
      wxa_free_str(&poll_response.body);
      continue;
    }

    sp_str_t login_status = wxa_json_get_string(poll_response.body.data, "status");
    if (sp_str_equal(login_status, sp_str_lit("confirmed"))) {
      sp_str_t bot_token = wxa_json_get_string(poll_response.body.data, "bot_token");
      sp_str_t raw_account_id = wxa_json_get_string(poll_response.body.data, "ilink_bot_id");
      sp_str_t normalized_account_id = wxa_normalize_account_id(raw_account_id);
      sp_str_t user_id = wxa_json_get_string(poll_response.body.data, "ilink_user_id");
      sp_str_t resolved_base_url = wxa_json_get_string(poll_response.body.data, "baseurl");
      if (resolved_base_url.len == 0U) {
        resolved_base_url = sp_str_copy(base_url);
      }

      wxa_free_str(&client->bot_token);
      wxa_free_str(&client->account_id);
      wxa_free_str(&client->base_url);

      client->bot_token = sp_str_copy(bot_token);
      client->account_id = sp_str_copy(normalized_account_id);
      client->base_url = sp_str_copy(resolved_base_url);
      wxa_save_account_record(client, user_id);
      wxa_fill_login_result(result, normalized_account_id, user_id, bot_token, qrcode_url, qrcode, resolved_base_url);

      wxa_free_str(&qr_response.body);
      wxa_free_str(&poll_response.body);
      wxa_free_str(&qr_url);
      wxa_free_str(&bot_type);
      wxa_free_str(&base_url);
      wxa_free_str(&login_status);
      wxa_free_str(&raw_account_id);
      return WXA_OK;
    }

    if (sp_str_equal(login_status, sp_str_lit("scaned"))) {
      wxa_log(client, "QR scanned, confirm in WeChat");
    } else if (sp_str_equal(login_status, sp_str_lit("expired"))) {
      wxa_free_str(&poll_response.body);
      wxa_free_str(&login_status);
      wxa_free_str(&qrcode);
      wxa_free_str(&qrcode_url);
      wxa_free_str(&qr_response.body);
      wxa_free_str(&qr_url);
      wxa_free_str(&bot_type);
      wxa_free_str(&base_url);
      return wxa_fail(client, WXA_ERR_TIMEOUT, "login QR expired");
    }

    wxa_free_str(&login_status);
    wxa_free_str(&poll_response.body);
    wxa_sleep_ms(1000U);
  }

  wxa_free_str(&qrcode);
  wxa_free_str(&qrcode_url);
  wxa_free_str(&qr_response.body);
  wxa_free_str(&qr_url);
  wxa_free_str(&bot_type);
  wxa_free_str(&base_url);
  return wxa_fail(client, WXA_ERR_TIMEOUT, "login timed out");
}

static wxa_status_t wxa_dispatch_message_segment(
  wxa_client_t* client,
  const wxa_agent_vtable_t* agent,
  void* user_data,
  sp_str_t segment
) {
  wxa_inbound_message_t inbound = {0};
  wxa_status_t status = wxa_parse_inbound_message(segment, &inbound);
  if (status != WXA_OK) {
    wxa_free_inbound_message(&inbound);
    return status;
  }

  if (inbound.from_user_id.len == 0U || (inbound.text.len == 0U && inbound.media_type == WXA_MEDIA_NONE)) {
    wxa_free_inbound_message(&inbound);
    return WXA_OK;
  }
  {
    sp_str_t msg = sp_format(
      "dispatch msg from={} ctx_len={} text_len={} media_type={}",
      SP_FMT_STR(inbound.from_user_id),
      SP_FMT_U32(inbound.context_token.len),
      SP_FMT_U32(inbound.text.len),
      SP_FMT_S64((s64)inbound.media_type)
    );
    wxa_log(client, msg.data);
    wxa_free_str(&msg);
  }

  wxa_chat_request_t request = {
    .conversation_id = inbound.from_user_id.data,
    .text = inbound.text.len > 0U ? inbound.text.data : "",
    .media = { .type = WXA_MEDIA_NONE, .file_path = NULL, .mime_type = NULL, .file_name = NULL }
  };
  wxa_chat_response_t response = {
    .text = NULL,
    .media = { .type = WXA_MEDIA_NONE, .file_path = NULL, .mime_type = NULL, .file_name = NULL }
  };
  sp_str_t typing_ticket = wxa_get_typing_ticket(
    client,
    inbound.from_user_id.data,
    inbound.context_token.data
  );
  wxa_send_typing(
    client,
    inbound.from_user_id.data,
    typing_ticket,
    WXA_TYPING_STATUS_TYPING
  );

  status = WXA_OK;
  if (inbound.media_type != WXA_MEDIA_NONE && inbound.media_encrypt_param.len > 0U) {
    status = wxa_download_media(
      client,
      inbound.media_type,
      inbound.media_encrypt_param,
      inbound.media_aes_key,
      inbound.media_hex_aes_key,
      inbound.media_file_name,
      &request.media
    );
    if (status != WXA_OK) {
      status = WXA_OK;
      request.media.type = WXA_MEDIA_NONE;
      request.media.file_path = NULL;
      request.media.mime_type = NULL;
      request.media.file_name = NULL;
    }
  }

  int rc = agent->chat(user_data, &request, &response);
  if (rc != 0) {
    sp_str_t msg = sp_format("agent callback failed with {}", SP_FMT_S32(rc));
    wxa_fail(client, WXA_ERR_CALLBACK, msg.data);
    sp_free((void*)msg.data);
    wxa_send_typing(
      client,
      inbound.from_user_id.data,
      typing_ticket,
      WXA_TYPING_STATUS_CANCEL
    );
    wxa_free_str(&typing_ticket);
    wxa_free_inbound_message(&inbound);
    return WXA_ERR_CALLBACK;
  }
  {
    sp_str_t msg = sp_format(
      "agent reply text_len={} media_type={}",
      SP_FMT_U32(response.text != NULL ? (u32)strlen(response.text) : 0U),
      SP_FMT_S64((s64)response.media.type)
    );
    wxa_log(client, msg.data);
    wxa_free_str(&msg);
  }

  if (response.media.type != WXA_MEDIA_NONE && response.media.file_path != NULL) {
    wxa_uploaded_media_t uploaded = {0};
    status = wxa_upload_media(client, &response.media, inbound.from_user_id.data, &uploaded);
    if (status == WXA_OK) {
      status = wxa_send_uploaded_media(
        client,
        inbound.from_user_id.data,
        inbound.context_token.data,
        &response.media,
        &uploaded,
        response.text
      );
    }
    wxa_free_str(&uploaded.download_param);
    wxa_free_str(&uploaded.aeskey_hex);
  } else if (response.text != NULL && response.text[0] != '\0') {
    status = wxa_send_text(client, inbound.from_user_id.data, inbound.context_token.data, response.text);
  } else {
    wxa_log(client, "skip send: empty response");
  }

  wxa_send_typing(
    client,
    inbound.from_user_id.data,
    typing_ticket,
    WXA_TYPING_STATUS_CANCEL
  );
  wxa_free_str(&typing_ticket);
  wxa_free_inbound_message(&inbound);
  if (request.media.file_path != NULL) {
    sp_free((void*)request.media.file_path);
  }
  {
    sp_str_t msg = sp_format("dispatch done status={}", SP_FMT_CSTR(wxa_status_message(status)));
    wxa_log(client, msg.data);
    wxa_free_str(&msg);
  }
  return status;
}

static wxa_status_t wxa_process_updates(
  wxa_client_t* client,
  const wxa_agent_vtable_t* agent,
  void* user_data,
  sp_str_t body
) {
  sp_str_t new_sync_buf = wxa_json_get_top_level_string(body, "get_updates_buf");
  if (new_sync_buf.len > 0U) {
    wxa_log_sync_state(client, "recv", new_sync_buf);
  }

  bool found_ret = false;
  long ret = wxa_json_get_top_level_long(body, "ret", &found_ret);
  if (found_ret && ret != 0L) {
    sp_str_t msg = sp_format("getupdates returned ret={}", SP_FMT_S64((s64)ret));
    wxa_status_t status = wxa_fail(client, WXA_ERR_PROTOCOL, msg.data);
    sp_free((void*)msg.data);
    wxa_free_str(&new_sync_buf);
    return status;
  }

  char* msgs = wxa_find_top_level_array(body, "msgs");
  if (msgs == NULL) {
    if (new_sync_buf.len > 0U) {
      wxa_free_str(&client->sync_buf);
      client->sync_buf = sp_str_copy(new_sync_buf);
      wxa_log_sync_state(client, "copy", client->sync_buf);
      wxa_save_sync_buf(client);
    }
    wxa_free_str(&new_sync_buf);
    return WXA_OK;
  }
  const char* msgs_end = wxa_find_matching_json(msgs, body.data + body.len, '[', ']');
  if (msgs_end == NULL) {
    wxa_free_str(&new_sync_buf);
    return wxa_fail(client, WXA_ERR_PROTOCOL, "invalid msgs array");
  }

  const char* cursor = msgs + 1;
  wxa_status_t process_status = WXA_OK;
  while (cursor < msgs_end) {
    sp_str_t segment = sp_str_lit("");
    bool found_seq = false;
    bool found_message_id = false;
    long seq = 0L;
    long message_id = 0L;
    if (!wxa_json_next_object(&cursor, msgs_end, &segment)) {
      break;
    }
    seq = wxa_json_get_long_range(segment.data, "seq", &found_seq, segment.data + segment.len);
    message_id = wxa_json_get_long_range(segment.data, "message_id", &found_message_id, segment.data + segment.len);
    if (found_seq && found_message_id &&
        (seq < client->last_seq || (seq == client->last_seq && message_id <= client->last_message_id))) {
      sp_str_t msg = sp_format(
        "skip replayed msg seq={} message_id={}",
        SP_FMT_S64((s64)seq),
        SP_FMT_S64((s64)message_id)
      );
      wxa_log(client, msg.data);
      wxa_free_str(&msg);
      continue;
    }
    wxa_status_t dispatch_status = wxa_dispatch_message_segment(client, agent, user_data, segment);
    if (dispatch_status != WXA_OK) {
      if (dispatch_status == WXA_ERR_NETWORK || dispatch_status == WXA_ERR_TIMEOUT) {
        // Do not block the whole updates stream on one transient reply failure.
        sp_str_t msg = sp_format(
          "dispatch transient failure status={} seq={} message_id={}, skip replay",
          SP_FMT_CSTR(wxa_status_message(dispatch_status)),
          SP_FMT_S64((s64)(found_seq ? seq : 0L)),
          SP_FMT_S64((s64)(found_message_id ? message_id : 0L))
        );
        wxa_log(client, msg.data);
        wxa_free_str(&msg);
      } else {
        process_status = dispatch_status;
        break;
      }
    }
    if (found_seq && found_message_id) {
      client->last_seq = seq;
      client->last_message_id = message_id;
    }
  }

  if (process_status == WXA_OK && new_sync_buf.len > 0U) {
    wxa_free_str(&client->sync_buf);
    client->sync_buf = sp_str_copy(new_sync_buf);
    wxa_log_sync_state(client, "copy", client->sync_buf);
    wxa_save_sync_buf(client);
  }
  wxa_free_str(&new_sync_buf);
  return process_status;
}

static bool wxa_is_session_expired_response(sp_str_t body) {
  bool found_ret = false;
  bool found_errcode = false;
  long ret = wxa_json_get_top_level_long(body, "ret", &found_ret);
  long errcode = wxa_json_get_top_level_long(body, "errcode", &found_errcode);
  return (found_ret && ret == WXA_SESSION_EXPIRED_ERRCODE) ||
         (found_errcode && errcode == WXA_SESSION_EXPIRED_ERRCODE);
}

static bool wxa_is_api_error_response(sp_str_t body, long* out_ret, long* out_errcode) {
  bool found_ret = false;
  bool found_errcode = false;
  long ret = wxa_json_get_top_level_long(body, "ret", &found_ret);
  long errcode = wxa_json_get_top_level_long(body, "errcode", &found_errcode);
  if (out_ret != NULL) *out_ret = found_ret ? ret : 0L;
  if (out_errcode != NULL) *out_errcode = found_errcode ? errcode : 0L;
  return (found_ret && ret != 0L) || (found_errcode && errcode != 0L);
}

static unsigned int wxa_random_range_ms(unsigned int min_ms, unsigned int max_ms) {
  if (max_ms <= min_ms) {
    return min_ms;
  }
  unsigned int span = max_ms - min_ms + 1U;
  unsigned int value = 0U;
  if (RAND_bytes((unsigned char*)&value, (int)sizeof(value)) != 1) {
    value = (unsigned int)(wxa_now_millis() & 0xFFFFFFFFULL);
  }
  return min_ms + (value % span);
}

static void wxa_handle_monitor_backoff(wxa_client_t* client, unsigned int* consecutive_failures) {
  unsigned int failures = 1U;
  unsigned int sleep_ms = 0U;
  if (consecutive_failures != NULL) {
    (*consecutive_failures)++;
    failures = *consecutive_failures;
  }
  if (failures <= 1U) {
    sleep_ms = wxa_random_range_ms(20U, 80U);
  } else if (failures == 2U) {
    sleep_ms = wxa_random_range_ms(80U, 180U);
  } else if (failures == 3U) {
    sleep_ms = wxa_random_range_ms(180U, 350U);
  } else {
    sleep_ms = wxa_random_range_ms(250U, 600U);
  }
  sp_str_t msg = sp_format("loop error count={}", SP_FMT_U32(failures));
  wxa_log(client, msg.data);
  wxa_free_str(&msg);
  {
    sp_str_t delay_msg = sp_format("loop backoff sleep_ms={}", SP_FMT_U32(sleep_ms));
    wxa_log(client, delay_msg.data);
    wxa_free_str(&delay_msg);
  }
  wxa_sleep_ms(sleep_ms);
  if (consecutive_failures != NULL && *consecutive_failures >= 8U) {
    *consecutive_failures = 0U;
  }
}

static sp_str_t wxa_build_getupdates_body(wxa_client_t* client) {
  sp_str_t sync_buf_json = wxa_json_escape(client->sync_buf.data != NULL ? client->sync_buf.data : "");
  char* sync_buf_json_c = sp_str_to_cstr(sync_buf_json);
  sp_str_t body = wxa_printf(
    "{\"get_updates_buf\":\"%s\",\"base_info\":{\"channel_version\":\"0.1.0\"}}",
    sync_buf_json_c
  );
  wxa_free_str(&sync_buf_json);
  sp_free(sync_buf_json_c);
  return body;
}

static void wxa_refresh_sync_buf_from_disk(wxa_client_t* client) {
  if (client == NULL || client->account_id.len == 0U) {
    return;
  }
  sp_str_t sync_path = wxa_sync_buf_path(client, client->account_id);
  sp_str_t sync_body = wxa_read_text_file(sync_path);
  if (sync_body.len > 0U) {
    sp_str_t sync = wxa_json_get_string(sync_body.data, "get_updates_buf");
    if (sync.len > 0U) {
      wxa_log_sync_state(client, "load", sync);
      wxa_free_str(&client->sync_buf);
      client->sync_buf = sp_str_copy(sync);
    }
    bool found_last_seq = false;
    bool found_last_message_id = false;
    long persisted_last_seq = wxa_json_get_top_level_long(sync_body, "last_seq", &found_last_seq);
    long persisted_last_message_id = wxa_json_get_top_level_long(sync_body, "last_message_id", &found_last_message_id);
    if (found_last_seq && found_last_message_id) {
      client->last_seq = persisted_last_seq;
      client->last_message_id = persisted_last_message_id;
    }
    wxa_free_str(&sync);
  }
  wxa_free_str(&sync_body);
  wxa_free_str(&sync_path);
}

static void wxa_log_sync_state(wxa_client_t* client, const char* phase, sp_str_t sync_buf) {
  sp_str_t preview = wxa_log_preview(sync_buf);
  sp_str_t msg = sp_format(
    "{} sync_len={} sync_preview={}",
    SP_FMT_CSTR(phase),
    SP_FMT_U32(sync_buf.len),
    SP_FMT_STR(preview)
  );
  wxa_log(client, msg.data);
  wxa_free_str(&msg);
  wxa_free_str(&preview);
}

static void wxa_log_updates_response(wxa_client_t* client, sp_str_t body) {
  char* msgs = wxa_find_top_level_array(body, "msgs");
  if (msgs == NULL) {
    return;
  }
  const char* end = body.data + body.len;
  const char* msgs_end = wxa_find_matching_json(msgs, end, '[', ']');
  if (msgs_end == NULL || msgs_end <= msgs + 1) {
    return;
  }
  sp_str_t preview = wxa_log_preview(body);
  sp_str_t msg = sp_format("recv body_preview={}", SP_FMT_STR(preview));
  wxa_log(client, msg.data);
  wxa_free_str(&msg);
  wxa_free_str(&preview);

  if (client != NULL && client->account_id.len > 0U) {
    sp_str_t path = wxa_join_path2(client->state_dir, "last-getupdates-with-msgs.json");
    (void)wxa_write_text_file(path, body);
    wxa_free_str(&path);
  }
}

wxa_status_t wxa_client_run(
  wxa_client_t* client,
  const wxa_agent_vtable_t* agent,
  void* user_data,
  const wxa_start_options_t* options
) {
  if (client == NULL || agent == NULL || agent->chat == NULL) {
    return wxa_fail(client, WXA_ERR_ARGUMENT, "client and agent.chat are required");
  }
  if (client->bot_token.len == 0U) {
    return wxa_fail(client, WXA_ERR_ARGUMENT, "bot token is required before run");
  }
  {
    wxa_status_t lock_status = wxa_acquire_monitor_lock(client);
    if (lock_status != WXA_OK) {
      return lock_status;
    }
  }

  unsigned int timeout_ms = options != NULL && options->long_poll_timeout_ms > 0U
    ? options->long_poll_timeout_ms
    : WXA_DEFAULT_LONG_POLL_TIMEOUT_MS;
  unsigned int next_timeout_ms = timeout_ms;
  unsigned int consecutive_failures = 0U;
  unsigned int consecutive_ret_minus1 = 0U;
  unsigned int retry_only_ticks = 0U;
  unsigned long long last_sync_reset_at_ms = 0ULL;
  unsigned long long last_msgs_at_ms = 0ULL;
  client->stop_requested = false;
  wxa_log(client, "weixin monitor started");

  while (!client->stop_requested) {
    wxa_process_retry_sends(client, 24U);
    if (sp_dyn_array_size(client->retry_sends) > 0U) {
      retry_only_ticks++;
      if (retry_only_ticks <= 3U) {
        wxa_sleep_ms(wxa_random_range_ms(60U, 140U));
        continue;
      }
      retry_only_ticks = 0U;
    } else {
      retry_only_ticks = 0U;
    }
    wxa_refresh_sync_buf_from_disk(client);
    wxa_log_sync_state(client, "send", client->sync_buf);
    sp_str_t body = wxa_build_getupdates_body(client);
    sp_str_t url = wxa_build_url(client->base_url, "ilink/bot/getupdates");
    wxa_http_response_t response = {0};
    unsigned int effective_timeout_ms = next_timeout_ms;
    if (sp_dyn_array_size(client->retry_sends) > 0U && effective_timeout_ms > 1500U) {
      effective_timeout_ms = 1500U;
    }
    wxa_status_t status = wxa_http_post_json(client, url, body.data, (long)effective_timeout_ms, &response);
    if (status != WXA_OK) {
      wxa_free_str(&response.body);
      wxa_free_str(&url);
      wxa_free_str(&body);
      if (sp_dyn_array_size(client->retry_sends) > 0U) {
        wxa_process_retry_sends(client, 48U);
        wxa_sleep_ms(wxa_random_range_ms(80U, 180U));
        consecutive_failures = 0U;
        continue;
      }
      wxa_handle_monitor_backoff(client, &consecutive_failures);
      continue;
    }

    bool found_timeout = false;
    long server_timeout = wxa_json_get_top_level_long(response.body, "longpolling_timeout_ms", &found_timeout);
    if (found_timeout && server_timeout > 0L) {
      next_timeout_ms = (unsigned int)server_timeout;
    }

    if (wxa_is_session_expired_response(response.body)) {
      wxa_log(client, "session expired, pausing 60 min before retry");
      consecutive_failures = 0U;
      next_timeout_ms = timeout_ms;
      wxa_free_str(&response.body);
      wxa_free_str(&url);
      wxa_free_str(&body);
      wxa_sleep_ms(WXA_SESSION_EXPIRED_PAUSE_MS);
      continue;
    }

    long api_ret = 0L;
    long api_errcode = 0L;
    if (wxa_is_api_error_response(response.body, &api_ret, &api_errcode)) {
      if (api_ret == -1L && api_errcode == 0L) {
        consecutive_ret_minus1++;
        if (consecutive_ret_minus1 % 10U == 0U) {
          sp_str_t msg = sp_format("getupdates transient ret=-1 count={}", SP_FMT_U32(consecutive_ret_minus1));
          wxa_log(client, msg.data);
          wxa_free_str(&msg);
        }
        if (consecutive_ret_minus1 >= 1U) {
          unsigned long long now_ms = wxa_now_millis();
          bool reset_cooldown_passed = last_sync_reset_at_ms == 0ULL || now_ms - last_sync_reset_at_ms >= 5000ULL;
          bool recently_received_msgs = last_msgs_at_ms > 0ULL && now_ms - last_msgs_at_ms < 1500ULL;
          if (reset_cooldown_passed && !recently_received_msgs) {
            wxa_log(client, "ret=-1 persists; reset get_updates_buf once for recovery");
            wxa_free_str(&client->sync_buf);
            wxa_clear_sync_buf(client);
            next_timeout_ms = timeout_ms;
            last_sync_reset_at_ms = now_ms;
            consecutive_ret_minus1 = 0U;
          }
        }
        // Frequent transient response; avoid busy-looping the long-poll endpoint.
        wxa_sleep_ms(120U);
        wxa_free_str(&response.body);
        wxa_free_str(&url);
        wxa_free_str(&body);
        consecutive_failures = 0U;
        continue;
      }
      consecutive_ret_minus1 = 0U;
      sp_str_t msg = sp_format(
        "getupdates api error ret={} errcode={} body={}",
        SP_FMT_S64((s64)api_ret),
        SP_FMT_S64((s64)api_errcode),
        SP_FMT_STR(response.body)
      );
      wxa_log(client, msg.data);
      wxa_free_str(&msg);
      wxa_free_str(&response.body);
      wxa_free_str(&url);
      wxa_free_str(&body);
      if (sp_dyn_array_size(client->retry_sends) > 0U) {
        wxa_process_retry_sends(client, 48U);
        wxa_sleep_ms(wxa_random_range_ms(80U, 180U));
        consecutive_failures = 0U;
        continue;
      }
      wxa_handle_monitor_backoff(client, &consecutive_failures);
      continue;
    }

    bool had_msgs = false;
    {
      char* msgs = wxa_find_top_level_array(response.body, "msgs");
      if (msgs != NULL) {
        const char* msgs_end = wxa_find_matching_json(msgs, response.body.data + response.body.len, '[', ']');
        had_msgs = msgs_end != NULL && msgs_end > msgs + 1;
      }
    }
    wxa_log_updates_response(client, response.body);
    status = wxa_process_updates(client, agent, user_data, response.body);
    wxa_free_str(&response.body);
    wxa_free_str(&url);
    wxa_free_str(&body);
    if (status == WXA_OK) {
      if (had_msgs) {
        last_msgs_at_ms = wxa_now_millis();
      }
      wxa_process_retry_sends(client, 64U);
      consecutive_ret_minus1 = 0U;
      consecutive_failures = 0U;
      continue;
    }

    sp_str_t msg = sp_format("loop error: {}", SP_FMT_CSTR(wxa_status_message(status)));
    wxa_log(client, msg.data);
    wxa_free_str(&msg);
    wxa_handle_monitor_backoff(client, &consecutive_failures);
  }

  wxa_release_monitor_lock(client);
  wxa_log(client, "weixin monitor stopped");
  return WXA_OK;
}

void wxa_client_stop(wxa_client_t* client) {
  if (client == NULL) {
    return;
  }
  client->stop_requested = true;
}

const char* wxa_client_last_error(const wxa_client_t* client) {
  if (client == NULL || client->last_error.data == NULL) {
    return "";
  }
  return client->last_error.data;
}

static void wxa_selftest_expect(wxa_selftest_state_t* state, bool condition, const char* name) {
  if (state == NULL) {
    return;
  }
  state->total++;
  if (!condition) {
    state->failed++;
    fprintf(stderr, "selftest failed: %s\n", name);
  }
}

static void wxa_selftest_parse_segment(wxa_selftest_state_t* state) {
  static const char* segment_json =
    "{"
      "\"from_user_id\":\"user-a\","
      "\"context_token\":\"ctx-1\","
      "\"item_list\":["
        "{\"type\":1,\"text_item\":{\"text\":\"正文\"},\"ref_msg\":{\"title\":\"引用标题\",\"message_item\":{\"type\":4,\"file_item\":{\"media\":{\"encrypt_query_param\":\"ref-param\",\"aes_key\":\"ref-key\",\"encrypt_type\":1},\"file_name\":\"proof.pdf\",\"len\":\"42\"}}}},"
        "{\"type\":3,\"voice_item\":{\"text\":\"语音转写\"}}"
      "]"
    "}";
  wxa_inbound_message_t inbound = {0};
  wxa_status_t status = wxa_parse_inbound_message(sp_str_view(segment_json), &inbound);

  wxa_selftest_expect(state, status == WXA_OK, "parse_segment_status");
  wxa_selftest_expect(state, sp_str_equal(inbound.from_user_id, sp_str_lit("user-a")), "parse_segment_user");
  wxa_selftest_expect(state, sp_str_equal(inbound.context_token, sp_str_lit("ctx-1")), "parse_segment_context");
  wxa_selftest_expect(
    state,
    sp_str_equal(inbound.text, sp_str_lit("正文")),
    "parse_segment_text"
  );
  wxa_selftest_expect(state, inbound.media_type == WXA_MEDIA_FILE, "parse_segment_media_type");
  wxa_selftest_expect(
    state,
    sp_str_equal(inbound.media_encrypt_param, sp_str_lit("ref-param")),
    "parse_segment_media_param"
  );
  wxa_selftest_expect(
    state,
    sp_str_equal(inbound.media_file_name, sp_str_lit("proof.pdf")),
    "parse_segment_media_file"
  );

  wxa_free_inbound_message(&inbound);
}

static void wxa_selftest_fixture_parse(
  wxa_selftest_state_t* state,
  const char* fixture_path,
  const char* expected_user,
  const char* expected_context,
  const char* expected_text,
  wxa_media_type_t expected_media_type,
  const char* expected_media_param,
  const char* expected_media_file
) {
  sp_str_t body = wxa_read_text_file(sp_str_view(fixture_path));
  char* msgs = body.len > 0U ? wxa_find_array(body.data, "msgs") : NULL;
  const char* msgs_end = msgs != NULL ? wxa_find_matching_json(msgs, body.data + body.len, '[', ']') : NULL;
  const char* cursor = msgs != NULL ? msgs + 1 : NULL;
  sp_str_t segment = sp_str_lit("");
  wxa_inbound_message_t inbound = {0};
  wxa_status_t status = WXA_ERR_PROTOCOL;

  if (msgs_end != NULL && wxa_json_next_object(&cursor, msgs_end, &segment)) {
    status = wxa_parse_inbound_message(segment, &inbound);
  }

  wxa_selftest_expect(state, body.len > 0U, "fixture_body_loaded");
  wxa_selftest_expect(state, msgs != NULL, "fixture_msgs_found");
  wxa_selftest_expect(state, msgs_end != NULL, "fixture_msgs_closed");
  wxa_selftest_expect(state, status == WXA_OK, "fixture_parse_status");
  wxa_selftest_expect(state, sp_str_equal(inbound.from_user_id, sp_str_view(expected_user)), "fixture_user");
  wxa_selftest_expect(state, sp_str_equal(inbound.context_token, sp_str_view(expected_context)), "fixture_context");
  wxa_selftest_expect(state, sp_str_equal(inbound.text, sp_str_view(expected_text)), "fixture_text");
  wxa_selftest_expect(state, inbound.media_type == expected_media_type, "fixture_media_type");

  if (expected_media_param != NULL) {
    wxa_selftest_expect(
      state,
      sp_str_equal(inbound.media_encrypt_param, sp_str_view(expected_media_param)),
      "fixture_media_param"
    );
  }
  if (expected_media_file != NULL) {
    wxa_selftest_expect(
      state,
      sp_str_equal(inbound.media_file_name, sp_str_view(expected_media_file)),
      "fixture_media_file"
    );
  }

  wxa_free_inbound_message(&inbound);
  wxa_free_str(&body);
}

static void wxa_selftest_msgs_iteration(wxa_selftest_state_t* state) {
  static const char* body =
    "{"
      "\"msgs\":["
        "{\"from_user_id\":\"user-a\",\"item_list\":[{\"type\":1,\"text_item\":{\"text\":\"one\"}}]},"
        "{\"from_user_id\":\"user-b\",\"item_list\":[{\"type\":1,\"text_item\":{\"text\":\"two\"}}]}"
      "]"
    "}";
  char* msgs = wxa_find_array(body, "msgs");
  const char* msgs_end = msgs != NULL ? wxa_find_matching_json(msgs, body + strlen(body), '[', ']') : NULL;
  const char* cursor = msgs != NULL ? msgs + 1 : NULL;
  sp_str_t first = sp_str_lit("");
  sp_str_t second = sp_str_lit("");
  bool ok_first = msgs_end != NULL && wxa_json_next_object(&cursor, msgs_end, &first);
  bool ok_second = msgs_end != NULL && wxa_json_next_object(&cursor, msgs_end, &second);
  bool ok_third = msgs_end != NULL && wxa_json_next_object(&cursor, msgs_end, &second);
  sp_str_t first_user = ok_first ? wxa_json_get_string(first.data, "from_user_id") : sp_str_lit("");
  sp_str_t second_user = ok_second ? wxa_json_get_string(second.data, "from_user_id") : sp_str_lit("");

  wxa_selftest_expect(state, msgs != NULL, "msgs_array_found");
  wxa_selftest_expect(state, msgs_end != NULL, "msgs_array_closed");
  wxa_selftest_expect(state, ok_first, "msgs_first_object");
  wxa_selftest_expect(state, ok_second, "msgs_second_object");
  wxa_selftest_expect(state, !ok_third, "msgs_no_third_object");
  wxa_selftest_expect(state, sp_str_equal(first_user, sp_str_lit("user-a")), "msgs_first_user");
  wxa_selftest_expect(state, sp_str_equal(second_user, sp_str_lit("user-b")), "msgs_second_user");

  wxa_free_str(&first_user);
  wxa_free_str(&second_user);
}

static void wxa_selftest_bounded_lookup(wxa_selftest_state_t* state) {
  static const char* body =
    "{"
      "\"outer\":{"
        "\"first\":{\"title\":\"left\"},"
        "\"second\":{\"title\":\"right\"}"
      "}"
    "}";
  char* first = wxa_find_object_in_range(body, "first", body + strlen(body));
  sp_str_t first_slice = wxa_json_object_slice(first, body + strlen(body));
  sp_str_t title = wxa_json_get_string_after_range(
    first_slice.data,
    first_slice.data,
    "title",
    first_slice.data + first_slice.len
  );
  sp_str_t wrong = wxa_json_get_string_after_range(
    first_slice.data,
    first_slice.data,
    "missing",
    first_slice.data + first_slice.len
  );

  wxa_selftest_expect(state, first != NULL, "bounded_first_found");
  wxa_selftest_expect(state, first_slice.len > 0U, "bounded_first_slice");
  wxa_selftest_expect(state, sp_str_equal(title, sp_str_lit("left")), "bounded_first_title");
  wxa_selftest_expect(state, wrong.len == 0U, "bounded_missing_empty");

  wxa_free_str(&title);
  wxa_free_str(&wrong);
}

static void wxa_selftest_top_level_lookup(wxa_selftest_state_t* state) {
  static const char* body =
    "{"
      "\"ret\":0,"
      "\"msgs\":["
        "{"
          "\"ret\":999,"
          "\"get_updates_buf\":\"nested-buf\""
        "}"
      "],"
      "\"get_updates_buf\":\"top-buf\","
      "\"longpolling_timeout_ms\":42000"
    "}";
  sp_str_t view = sp_str_view(body);
  sp_str_t top_buf = wxa_json_get_top_level_string(view, "get_updates_buf");
  bool found_ret = false;
  bool found_timeout = false;
  long ret = wxa_json_get_top_level_long(view, "ret", &found_ret);
  long timeout = wxa_json_get_top_level_long(view, "longpolling_timeout_ms", &found_timeout);
  char* msgs = wxa_find_top_level_array(view, "msgs");

  wxa_selftest_expect(state, found_ret && ret == 0L, "top_level_ret");
  wxa_selftest_expect(state, found_timeout && timeout == 42000L, "top_level_timeout");
  wxa_selftest_expect(state, sp_str_equal(top_buf, sp_str_lit("top-buf")), "top_level_buf");
  wxa_selftest_expect(state, msgs != NULL && *msgs == '[', "top_level_msgs");

  wxa_free_str(&top_buf);
}

static void wxa_selftest_session_response(wxa_selftest_state_t* state) {
  wxa_selftest_expect(
    state,
    wxa_is_session_expired_response(sp_str_view("{\"ret\":-14}")),
    "session_ret_detected"
  );
  wxa_selftest_expect(
    state,
    wxa_is_session_expired_response(sp_str_view("{\"errcode\":-14}")),
    "session_errcode_detected"
  );
  wxa_selftest_expect(
    state,
    !wxa_is_session_expired_response(sp_str_view("{\"ret\":0,\"errcode\":0}")),
    "session_nonexpired_ignored"
  );
}

int wxa_selftest_run(void) {
  wxa_selftest_state_t state = {0};

  wxa_selftest_parse_segment(&state);
  wxa_selftest_fixture_parse(
    &state,
    "tests/fixtures/getupdates-text.json",
    "user-text",
    "ctx-text",
    "hello from fixture",
    WXA_MEDIA_NONE,
    NULL,
    NULL
  );
  wxa_selftest_fixture_parse(
    &state,
    "tests/fixtures/getupdates-quoted-text.json",
    "user-quote",
    "ctx-quote",
    "[引用: 引用标题 | 被引用正文]\n回复正文",
    WXA_MEDIA_NONE,
    NULL,
    NULL
  );
  wxa_selftest_fixture_parse(
    &state,
    "tests/fixtures/getupdates-quoted-file.json",
    "user-file",
    "ctx-file",
    "请看附件",
    WXA_MEDIA_FILE,
    "quoted-file-param",
    "evidence.pdf"
  );
  wxa_selftest_fixture_parse(
    &state,
    "tests/fixtures/getupdates-direct-media-priority.json",
    "user-media",
    "ctx-media",
    "",
    WXA_MEDIA_IMAGE,
    "image-param",
    ""
  );
  wxa_selftest_fixture_parse(
    &state,
    "tests/fixtures/getupdates-voice-transcript.json",
    "user-voice",
    "ctx-voice",
    "这是语音转写",
    WXA_MEDIA_NONE,
    NULL,
    NULL
  );
  wxa_selftest_msgs_iteration(&state);
  wxa_selftest_bounded_lookup(&state);
  wxa_selftest_top_level_lookup(&state);
  wxa_selftest_session_response(&state);

  if (state.failed == 0) {
    fprintf(stdout, "selftest ok: %d checks\n", state.total);
    return 0;
  }

  fprintf(stderr, "selftest failed: %d/%d checks\n", state.failed, state.total);
  return 1;
}
