#include "weixin_agent_sdk.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wxa_client_t* g_client = NULL;

static void on_signal(int signum) {
  (void)signum;
  if (g_client != NULL) {
    wxa_client_stop(g_client);
  }
}

static void log_line(void* user_data, const char* message) {
  (void)user_data;
  fprintf(stderr, "[echo-bot] %s\n", message);
}

static int echo_chat(void* user_data, const wxa_chat_request_t* request, wxa_chat_response_t* response) {
  (void)user_data;
  static char reply[4096];
  snprintf(reply, sizeof(reply), "你说了: %s", request->text != NULL ? request->text : "");
  response->text = reply;
  return 0;
}

int main(void) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  wxa_client_options_t client_options = {
    .base_url = NULL,
    .bot_token = getenv("WEIXIN_BOT_TOKEN"),
    .account_id = getenv("WEIXIN_ACCOUNT_ID"),
    .log_fn = log_line,
    .log_user_data = NULL
  };
  g_client = wxa_client_new(&client_options);
  if (g_client == NULL) {
    fprintf(stderr, "failed to allocate client\n");
    return 1;
  }

  if (client_options.bot_token == NULL || client_options.bot_token[0] == '\0') {
    wxa_login_result_t login_result = {0};
    wxa_status_t login_status = wxa_client_login(g_client, NULL, &login_result);
    if (login_status != WXA_OK) {
      fprintf(stderr, "login failed: %s\n", wxa_client_last_error(g_client));
      wxa_client_free(g_client);
      return 1;
    }

    fprintf(stderr, "login ok account=%s user=%s\n",
      login_result.account_id != NULL ? login_result.account_id : "",
      login_result.user_id != NULL ? login_result.user_id : "");
  }

  wxa_agent_vtable_t agent = {
    .chat = echo_chat
  };
  wxa_status_t run_status = wxa_client_run(g_client, &agent, NULL, NULL);
  if (run_status != WXA_OK) {
    fprintf(stderr, "run failed: %s\n", wxa_client_last_error(g_client));
    wxa_client_free(g_client);
    return 1;
  }

  wxa_client_free(g_client);
  return 0;
}
