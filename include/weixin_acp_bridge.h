#ifndef WEIXIN_ACP_BRIDGE_H
#define WEIXIN_ACP_BRIDGE_H

#include "weixin_agent_sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wxa_acp_agent wxa_acp_agent_t;

typedef struct {
  const char* command;
  const char* const* args;
  int arg_count;
  const char* cwd;
  unsigned int prompt_timeout_ms;
} wxa_acp_agent_options_t;

wxa_acp_agent_t* wxa_acp_agent_new(const wxa_acp_agent_options_t* options);
void wxa_acp_agent_free(wxa_acp_agent_t* agent);

int wxa_acp_agent_chat(
  wxa_acp_agent_t* agent,
  const wxa_chat_request_t* request,
  wxa_chat_response_t* response
);

const char* wxa_acp_agent_last_error(const wxa_acp_agent_t* agent);

#ifdef __cplusplus
}
#endif

#endif
