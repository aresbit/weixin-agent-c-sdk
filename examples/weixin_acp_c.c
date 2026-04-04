#include "weixin_acp_bridge.h"
#include "weixin_agent_sdk.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#ifdef WXA_HAVE_QRENCODE
#include <qrencode.h>
#endif

typedef struct {
  wxa_acp_agent_t* bridge;
} wxa_app_t;

static wxa_client_t* g_client = NULL;
static const char* WXA_QR_PREFIX = "scan this QR URL with WeChat: ";

#ifdef WXA_HAVE_QRENCODE
static int wxa_qr_module(const QRcode* qr, int x, int y, int margin) {
  int width = qr->width;
  if (x < margin || y < margin || x >= width + margin || y >= width + margin) {
    return 0;
  }
  int qx = x - margin;
  int qy = y - margin;
  return (qr->data[qy * width + qx] & 0x1) != 0;
}

static int wxa_print_qr_utf8(const char* content) {
  const int margin = 2;
  QRcode* qr = QRcode_encodeString8bit(content, 0, QR_ECLEVEL_M);
  if (qr == NULL) {
    return -1;
  }

  int full = qr->width + margin * 2;
  fprintf(stderr, "\n");
  for (int y = 0; y < full; ++y) {
    for (int x = 0; x < full; ++x) {
      fputs(wxa_qr_module(qr, x, y, margin) ? "██" : "  ", stderr);
    }
    fputc('\n', stderr);
  }
  fputc('\n', stderr);
  QRcode_free(qr);
  return 0;
}
#endif

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
#ifdef WXA_HAVE_QRENCODE
  if (wxa_print_qr_utf8(url) != 0) {
    fprintf(stderr, "failed to render UTF QR in-process\n");
  }
#else
  fprintf(stderr, "qrencode dev library not linked; install libqrencode-dev and rebuild.\n");
#endif
}

static void wxa_handle_signal(int signo) {
  (void)signo;
  if (g_client != NULL) {
    wxa_client_stop(g_client);
  }
}

static int wxa_bridge_chat(void* user_data, const wxa_chat_request_t* request, wxa_chat_response_t* response) {
  wxa_app_t* app = (wxa_app_t*)user_data;
  int rc;
  response->text = NULL;
  response->media = (wxa_media_t){0};
  if (app == NULL || app->bridge == NULL) {
    return -1;
  }
  rc = wxa_acp_agent_chat(app->bridge, request, response);
  if (rc != 0) {
    const char* err = wxa_acp_agent_last_error(app->bridge);
    if (err != NULL && err[0] != '\0') {
      fprintf(stderr, "acp chat failed: %s\n", err);
    }
  }
  return rc;
}

static int wxa_command_exists(const char* command) {
  if (command == NULL || command[0] == '\0') {
    return 0;
  }
  if (strchr(command, '/') != NULL) {
    return access(command, X_OK) == 0;
  }
  const char* path_env = getenv("PATH");
  if (path_env == NULL || path_env[0] == '\0') {
    return 0;
  }
  const char* p = path_env;
  while (*p != '\0') {
    const char* end = strchr(p, ':');
    size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
    char candidate[PATH_MAX];
    if (len == 0U) {
      p = end != NULL ? end + 1 : p + len;
      continue;
    }
    if (len + 1U + strlen(command) + 1U < sizeof(candidate)) {
      memcpy(candidate, p, len);
      candidate[len] = '/';
      strcpy(candidate + len + 1U, command);
      if (access(candidate, X_OK) == 0) {
        return 1;
      }
    }
    if (end == NULL) {
      break;
    }
    p = end + 1;
  }
  return 0;
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
    "  %s start -- claude-agent-acp\n"
    "  %s start -- npx @zed-industries/codex-acp\n",
    argv0,
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
  const char* acp_command = NULL;
  wxa_client_t* client;
  wxa_acp_agent_t* bridge;
  wxa_app_t app;
  wxa_agent_vtable_t agent = { .chat = wxa_bridge_chat };
  wxa_acp_agent_options_t bridge_opts = {0};

  if (dd_index + 1 >= argc) {
    fprintf(stderr, "missing ACP command after --\n");
    return 1;
  }

  acp_command = argv[dd_index + 1];
  if (strcmp(acp_command, "claude-code-acp") == 0 && !wxa_command_exists("claude-code-acp")) {
    if (wxa_command_exists("claude-agent-acp")) {
      fprintf(stderr, "fallback ACP command: claude-code-acp -> claude-agent-acp\n");
      acp_command = "claude-agent-acp";
    }
  }

  bridge_opts = (wxa_acp_agent_options_t){
    .command = acp_command,
    .args = (const char* const*)&argv[dd_index + 2],
    .arg_count = argc - dd_index - 2,
    .cwd = cwd != NULL ? cwd : NULL,
    .prompt_timeout_ms = 120000U
  };

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
