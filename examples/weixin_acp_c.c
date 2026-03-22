#include "weixin_acp_bridge.h"
#include "weixin_agent_sdk.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  wxa_acp_agent_t* bridge;
} wxa_app_t;

static wxa_client_t* g_client = NULL;
static const char* WXA_QR_PREFIX = "scan this QR URL with WeChat: ";

static void wxa_cli_log(void* user_data, const char* message) {
  (void)user_data;
  if (message == NULL) {
    return;
  }
  fprintf(stderr, "%s\n", message);
  if (strncmp(message, WXA_QR_PREFIX, strlen(WXA_QR_PREFIX)) != 0) {
    return;
  }
  const char* url = message + strlen(WXA_QR_PREFIX);
  if (url[0] == '\0') {
    return;
  }
  char cmd[2048];
  int n = snprintf(cmd, sizeof(cmd), "qrencode -t UTF8 '%s'", url);
  if (n > 0 && (size_t)n < sizeof(cmd)) {
    (void)system(cmd);
  }
}

static void wxa_handle_signal(int signo) {
  (void)signo;
  if (g_client != NULL) {
    wxa_client_stop(g_client);
  }
}

static int wxa_bridge_chat(void* user_data, const wxa_chat_request_t* request, wxa_chat_response_t* response) {
  wxa_app_t* app = (wxa_app_t*)user_data;
  response->text = NULL;
  response->media = (wxa_media_t){0};
  if (app == NULL || app->bridge == NULL) {
    return -1;
  }
  return wxa_acp_agent_chat(app->bridge, request, response);
}

static void wxa_print_usage(const char* argv0) {
  fprintf(
    stderr,
    "weixin_acp_c - WeChat + ACP bridge (C)\n\n"
    "Usage:\n"
    "  %s login\n"
    "  %s start -- <acp-command> [args...]\n\n"
    "Examples:\n"
    "  %s start -- codex-acp\n"
    "  %s start -- npx @zed-industries/codex-acp\n",
    argv0,
    argv0,
    argv0,
    argv0
  );
}

static int wxa_do_login(void) {
  wxa_client_t* client = wxa_client_new(&(wxa_client_options_t){
    .log_fn = wxa_cli_log
  });
  wxa_login_result_t result = {0};
  int rc = 1;
  if (client == NULL) {
    fprintf(stderr, "failed to create client\n");
    return 1;
  }
  if (wxa_client_login(client, NULL, &result) == WXA_OK) {
    fprintf(stdout, "login ok: account=%s user=%s\n", result.account_id, result.user_id);
    rc = 0;
  } else {
    fprintf(stderr, "login failed: %s\n", wxa_client_last_error(client));
  }
  wxa_client_free(client);
  return rc;
}

static int wxa_do_start(int argc, char** argv, int dd_index) {
  const char* cwd = getenv("ACP_CWD");
  wxa_client_t* client;
  wxa_acp_agent_t* bridge;
  wxa_app_t app;
  wxa_agent_vtable_t agent = { .chat = wxa_bridge_chat };
  wxa_acp_agent_options_t bridge_opts = {
    .command = argv[dd_index + 1],
    .args = (const char* const*)&argv[dd_index + 2],
    .arg_count = argc - dd_index - 2,
    .cwd = cwd != NULL ? cwd : NULL,
    .prompt_timeout_ms = 120000U
  };

  if (dd_index + 1 >= argc) {
    fprintf(stderr, "missing ACP command after --\n");
    return 1;
  }

  client = wxa_client_new(&(wxa_client_options_t){
    .log_fn = wxa_cli_log
  });
  if (client == NULL) {
    fprintf(stderr, "failed to create client\n");
    return 1;
  }
  if (wxa_client_last_error(client)[0] != '\0') {
    fprintf(stderr, "client init warning: %s\n", wxa_client_last_error(client));
  }

  bridge = wxa_acp_agent_new(&bridge_opts);
  if (bridge == NULL) {
    fprintf(stderr, "failed to create ACP bridge\n");
    wxa_client_free(client);
    return 1;
  }

  app.bridge = bridge;
  g_client = client;
  signal(SIGINT, wxa_handle_signal);
  signal(SIGTERM, wxa_handle_signal);

  if (wxa_client_run(client, &agent, &app, NULL) != WXA_OK) {
    fprintf(stderr, "run failed: %s\n", wxa_client_last_error(client));
    if (wxa_acp_agent_last_error(bridge)[0] != '\0') {
      fprintf(stderr, "acp failed: %s\n", wxa_acp_agent_last_error(bridge));
    }
    wxa_acp_agent_free(bridge);
    wxa_client_free(client);
    g_client = NULL;
    return 1;
  }

  wxa_acp_agent_free(bridge);
  wxa_client_free(client);
  g_client = NULL;
  return 0;
}

int main(int argc, char** argv) {
  int dd_index = -1;
  if (argc < 2) {
    wxa_print_usage(argv[0]);
    return 1;
  }

  if (strcmp(argv[1], "login") == 0) {
    return wxa_do_login();
  }

  if (strcmp(argv[1], "start") != 0) {
    wxa_print_usage(argv[0]);
    return 1;
  }

  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--") == 0) {
      dd_index = i;
      break;
    }
  }
  if (dd_index < 0) {
    wxa_print_usage(argv[0]);
    return 1;
  }
  return wxa_do_start(argc, argv, dd_index);
}
