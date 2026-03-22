#ifndef WEIXIN_AGENT_SDK_H
#define WEIXIN_AGENT_SDK_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wxa_client wxa_client_t;

typedef enum {
  WXA_OK = 0,
  WXA_ERR_ARGUMENT = 1,
  WXA_ERR_NETWORK = 2,
  WXA_ERR_PROTOCOL = 3,
  WXA_ERR_TIMEOUT = 4,
  WXA_ERR_CALLBACK = 5
} wxa_status_t;

typedef enum {
  WXA_MEDIA_NONE = 0,
  WXA_MEDIA_IMAGE = 1,
  WXA_MEDIA_AUDIO = 2,
  WXA_MEDIA_VIDEO = 3,
  WXA_MEDIA_FILE = 4
} wxa_media_type_t;

typedef struct {
  wxa_media_type_t type;
  const char* file_path;
  const char* mime_type;
  const char* file_name;
} wxa_media_t;

typedef struct {
  const char* conversation_id;
  const char* text;
  wxa_media_t media;
} wxa_chat_request_t;

typedef struct {
  const char* text;
  wxa_media_t media;
} wxa_chat_response_t;

typedef struct {
  int (*chat)(void* user_data, const wxa_chat_request_t* request, wxa_chat_response_t* response);
} wxa_agent_vtable_t;

typedef void (*wxa_log_fn)(void* user_data, const char* message);

typedef struct {
  const char* base_url;
  const char* bot_token;
  const char* account_id;
  wxa_log_fn log_fn;
  void* log_user_data;
} wxa_client_options_t;

typedef struct {
  const char* base_url;
  const char* bot_type;
  unsigned int timeout_ms;
} wxa_login_options_t;

typedef struct {
  const char* account_id;
  const char* user_id;
  const char* bot_token;
  const char* qrcode_url;
  const char* session_qrcode;
  const char* base_url;
} wxa_login_result_t;

typedef struct {
  unsigned int long_poll_timeout_ms;
} wxa_start_options_t;

const char* wxa_version(void);

wxa_client_t* wxa_client_new(const wxa_client_options_t* options);
void wxa_client_free(wxa_client_t* client);

wxa_status_t wxa_client_login(
  wxa_client_t* client,
  const wxa_login_options_t* options,
  wxa_login_result_t* result
);

wxa_status_t wxa_client_run(
  wxa_client_t* client,
  const wxa_agent_vtable_t* agent,
  void* user_data,
  const wxa_start_options_t* options
);

void wxa_client_stop(wxa_client_t* client);
const char* wxa_client_last_error(const wxa_client_t* client);
int wxa_selftest_run(void);

#ifdef __cplusplus
}
#endif

#endif
