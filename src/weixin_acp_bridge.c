#include "weixin_acp_bridge.h"

#include <openssl/evp.h>
#include <sp.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define WXA_ACP_PROTOCOL_VERSION 1
#define WXA_ACP_DEFAULT_PROMPT_TIMEOUT_MS 120000U
#define WXA_ACP_MEDIA_OUT_DIR "/tmp/weixin-agent/media/acp-out"

typedef struct {
  sp_str_t conversation_id;
  sp_str_t session_id;
} wxa_acp_session_entry_t;

typedef struct {
  bool image;
  bool audio;
  bool embedded_context;
  bool load_session;
} wxa_acp_capabilities_t;

typedef struct {
  sp_str_t text;
  sp_str_t image_base64;
  sp_str_t image_mime_type;
} wxa_acp_prompt_result_t;

typedef struct {
  long id;
  bool done;
  bool failed;
  sp_str_t payload;
} wxa_acp_pending_response_t;

struct wxa_acp_agent {
  sp_str_t command;
  sp_da(sp_str_t) args;
  sp_str_t cwd;
  sp_da(wxa_acp_session_entry_t) sessions;
  sp_str_t last_error;
  unsigned int prompt_timeout_ms;
  wxa_acp_capabilities_t capabilities;

  int stdin_fd;
  int stdout_fd;
  FILE* stdout_file;
  pid_t child_pid;
  pthread_t reader_thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool ready;
  bool stop_reader;
  bool prompt_active;
  sp_str_t active_session_id;
  wxa_acp_prompt_result_t prompt_result;
  wxa_acp_pending_response_t pending;
};

static void wxa_acp_free_str(sp_str_t* value) {
  if (value != NULL && value->data != NULL && value->len > 0U) {
    sp_free((void*)value->data);
  }
  if (value != NULL) {
    *value = sp_str_lit("");
  }
}

static void wxa_acp_set_error(wxa_acp_agent_t* agent, const char* message) {
  if (agent == NULL) {
    return;
  }
  wxa_acp_free_str(&agent->last_error);
  agent->last_error = sp_str_from_cstr(message != NULL ? message : "unknown acp error");
}

static sp_str_t wxa_acp_printf(const char* fmt, ...) {
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

static char* wxa_acp_find_after_key(const char* body, const char* key) {
  sp_str_t needle = sp_format("\"{}\"", SP_FMT_CSTR(key));
  size_t needle_len = (size_t)needle.len;
  char* match = NULL;
  for (const char* p = body; p != NULL && *p != '\0'; ++p) {
    if (strncmp(p, needle.data, needle_len) == 0) {
      match = (char*)p;
      break;
    }
  }
  wxa_acp_free_str(&needle);
  if (match == NULL) {
    return NULL;
  }
  match += (ptrdiff_t)needle_len;
  while (*match != '\0' && *match != ':') {
    match++;
  }
  return *match == ':' ? match + 1 : NULL;
}

static sp_str_t wxa_acp_json_get_string(const char* body, const char* key) {
  char* cursor = wxa_acp_find_after_key(body, key);
  sp_str_builder_t builder = SP_ZERO_INITIALIZE();
  if (cursor == NULL) {
    return sp_str_lit("");
  }
  while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t') {
    cursor++;
  }
  if (*cursor != '"') {
    return sp_str_lit("");
  }
  cursor++;
  while (*cursor != '\0') {
    if (*cursor == '"') {
      return sp_str_builder_to_str(&builder);
    }
    if (*cursor == '\\') {
      cursor++;
      if (*cursor == '\0') {
        return sp_str_builder_to_str(&builder);
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
        case 'u':
          cursor += 4;
          sp_str_builder_append_c8(&builder, '?');
          break;
        default:
          sp_str_builder_append_c8(&builder, *cursor);
          break;
      }
    } else {
      sp_str_builder_append_c8(&builder, *cursor);
    }
    cursor++;
  }
  return sp_str_builder_to_str(&builder);
}

static long wxa_acp_json_get_long(const char* body, const char* key, bool* found) {
  char* cursor = wxa_acp_find_after_key(body, key);
  char number_buf[64];
  size_t n = 0U;
  if (cursor == NULL) {
    if (found != NULL) {
      *found = false;
    }
    return 0L;
  }
  while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t') {
    cursor++;
  }
  if (*cursor == '-' || *cursor == '+') {
    number_buf[n++] = *cursor++;
  }
  while (*cursor >= '0' && *cursor <= '9' && n + 1U < sizeof(number_buf)) {
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

static bool wxa_acp_json_has_true(const char* body, const char* key) {
  char* cursor = wxa_acp_find_after_key(body, key);
  if (cursor == NULL) {
    return false;
  }
  while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t') {
    cursor++;
  }
  return strncmp(cursor, "true", 4) == 0;
}

static sp_str_t wxa_acp_json_escape(const char* input) {
  sp_str_builder_t builder = SP_ZERO_INITIALIZE();
  sp_str_t view = sp_str_view(input != NULL ? input : "");
  sp_str_for(view, i) {
    c8 c = view.data[i];
    switch (c) {
      case '\\': sp_str_builder_append_cstr(&builder, "\\\\"); break;
      case '"': sp_str_builder_append_cstr(&builder, "\\\""); break;
      case '\n': sp_str_builder_append_cstr(&builder, "\\n"); break;
      case '\r': sp_str_builder_append_cstr(&builder, "\\r"); break;
      case '\t': sp_str_builder_append_cstr(&builder, "\\t"); break;
      default: sp_str_builder_append_c8(&builder, c); break;
    }
  }
  return sp_str_builder_to_str(&builder);
}

static sp_str_t wxa_acp_read_file_base64(const char* file_path) {
  FILE* fp = fopen(file_path, "rb");
  if (fp == NULL) {
    return sp_str_lit("");
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    fclose(fp);
    return sp_str_lit("");
  }
  long size = ftell(fp);
  if (size < 0L) {
    fclose(fp);
    return sp_str_lit("");
  }
  rewind(fp);

  unsigned char* buf = sp_alloc((u32)size + 1U);
  int out_len = 4 * ((int)(((size_t)size + 2U) / 3U));
  unsigned char* out = sp_alloc((u32)out_len + 1U);
  size_t got = fread(buf, 1U, (size_t)size, fp);
  fclose(fp);
  EVP_EncodeBlock(out, buf, (int)got);
  out[out_len] = '\0';
  sp_free(buf);
  return sp_str((const char*)out, (u32)out_len);
}

static bool wxa_acp_ensure_dir(const char* path) {
  char* mutable_path = sp_str_to_cstr(sp_str_view(path));
  size_t len = strlen(mutable_path);
  for (size_t i = 1; i < len; ++i) {
    if (mutable_path[i] == '/') {
      mutable_path[i] = '\0';
      if (mkdir(mutable_path, 0700) != 0 && errno != EEXIST) {
        sp_free(mutable_path);
        return false;
      }
      mutable_path[i] = '/';
    }
  }
  if (mkdir(mutable_path, 0700) != 0 && errno != EEXIST) {
    sp_free(mutable_path);
    return false;
  }
  sp_free(mutable_path);
  return true;
}

static sp_str_t wxa_acp_guess_image_extension(sp_str_t mime_type) {
  if (sp_str_equal(mime_type, sp_str_lit("image/jpeg"))) return sp_str_lit("jpg");
  if (sp_str_equal(mime_type, sp_str_lit("image/webp"))) return sp_str_lit("webp");
  if (sp_str_equal(mime_type, sp_str_lit("image/gif"))) return sp_str_lit("gif");
  return sp_str_lit("png");
}

static sp_str_t wxa_acp_write_image_file(sp_str_t base64, sp_str_t mime_type) {
  if (!wxa_acp_ensure_dir(WXA_ACP_MEDIA_OUT_DIR)) {
    return sp_str_lit("");
  }

  int decoded_capacity = (int)((base64.len / 4U) * 3U + 4U);
  unsigned char* decoded = sp_alloc((u32)decoded_capacity);
  int decoded_len = EVP_DecodeBlock(decoded, (const unsigned char*)base64.data, (int)base64.len);
  if (decoded_len < 0) {
    sp_free(decoded);
    return sp_str_lit("");
  }
  while (base64.len > 0U && base64.data[base64.len - 1U] == '=') {
    decoded_len--;
    base64.len--;
  }

  sp_str_t ext = wxa_acp_guess_image_extension(mime_type);
  sp_str_t path = sp_format(
    "{}/acp-{}.{}",
    SP_FMT_CSTR(WXA_ACP_MEDIA_OUT_DIR),
    SP_FMT_S64((s64)getpid()),
    SP_FMT_STR(ext)
  );
  char* cpath = sp_str_to_cstr(path);
  FILE* fp = fopen(cpath, "wb");
  if (fp == NULL) {
    sp_free(cpath);
    sp_free(decoded);
    wxa_acp_free_str(&path);
    return sp_str_lit("");
  }
  (void)fwrite(decoded, 1U, (size_t)decoded_len, fp);
  fclose(fp);
  sp_free(cpath);
  sp_free(decoded);
  return path;
}

static bool wxa_acp_write_line(wxa_acp_agent_t* agent, sp_str_t line) {
  if (agent == NULL || agent->stdin_fd < 0) {
    return false;
  }
  ssize_t wrote = write(agent->stdin_fd, line.data, line.len);
  if (wrote < 0 || (size_t)wrote != line.len) {
    return false;
  }
  if (write(agent->stdin_fd, "\n", 1U) != 1) {
    return false;
  }
  return true;
}

static void wxa_acp_prompt_result_reset(wxa_acp_prompt_result_t* result) {
  if (result == NULL) {
    return;
  }
  wxa_acp_free_str(&result->text);
  wxa_acp_free_str(&result->image_base64);
  wxa_acp_free_str(&result->image_mime_type);
}

static void wxa_acp_append_text(sp_str_t* acc, sp_str_t text) {
  if (acc == NULL || text.len == 0U) {
    return;
  }
  if (acc->len == 0U) {
    *acc = sp_str_copy(text);
    return;
  }
  sp_str_t next = sp_format("{}{}", SP_FMT_STR(*acc), SP_FMT_STR(text));
  wxa_acp_free_str(acc);
  *acc = next;
}

static void wxa_acp_handle_prompt_update(wxa_acp_agent_t* agent, const char* line) {
  sp_str_t session_id = wxa_acp_json_get_string(line, "sessionId");
  if (!agent->prompt_active || !sp_str_equal(session_id, agent->active_session_id)) {
    wxa_acp_free_str(&session_id);
    return;
  }
  wxa_acp_free_str(&session_id);

  sp_str_t update_type = wxa_acp_json_get_string(line, "sessionUpdate");
  if (sp_str_equal(update_type, sp_str_lit("agent_message_chunk"))) {
    sp_str_t content_type = wxa_acp_json_get_string(line, "type");
    if (sp_str_equal(content_type, sp_str_lit("text"))) {
      sp_str_t text = wxa_acp_json_get_string(line, "text");
      wxa_acp_append_text(&agent->prompt_result.text, text);
      wxa_acp_free_str(&text);
    } else if (sp_str_equal(content_type, sp_str_lit("image"))) {
      wxa_acp_free_str(&agent->prompt_result.image_base64);
      wxa_acp_free_str(&agent->prompt_result.image_mime_type);
      agent->prompt_result.image_base64 = wxa_acp_json_get_string(line, "data");
      agent->prompt_result.image_mime_type = wxa_acp_json_get_string(line, "mimeType");
    }
    wxa_acp_free_str(&content_type);
  }
  wxa_acp_free_str(&update_type);
}

static void wxa_acp_handle_permission_request(wxa_acp_agent_t* agent, const char* line) {
  bool found_id = false;
  long request_id = wxa_acp_json_get_long(line, "id", &found_id);
  sp_str_t option_id = wxa_acp_json_get_string(line, "optionId");
  if (agent == NULL || !found_id || option_id.len == 0U) {
    wxa_acp_free_str(&option_id);
    return;
  }
  sp_str_t escaped_option = wxa_acp_json_escape(option_id.data);
  sp_str_t response = wxa_acp_printf(
    "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"outcome\":{\"outcome\":\"selected\",\"optionId\":\"%s\"}}}",
    request_id,
    escaped_option.data
  );
  (void)wxa_acp_write_line(agent, response);
  wxa_acp_free_str(&response);
  wxa_acp_free_str(&escaped_option);
  wxa_acp_free_str(&option_id);
}

static void* wxa_acp_reader_main(void* user_data) {
  wxa_acp_agent_t* agent = (wxa_acp_agent_t*)user_data;
  char* line = NULL;
  size_t cap = 0U;
  if (agent == NULL || agent->stdout_file == NULL) {
    return NULL;
  }

  while (!agent->stop_reader) {
    ssize_t got = getline(&line, &cap, agent->stdout_file);
    if (got < 0) {
      break;
    }
    while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r')) {
      line[--got] = '\0';
    }
    pthread_mutex_lock(&agent->mutex);
    bool is_permission = strstr(line, "\"method\"") != NULL && strstr(line, "session/request_permission") != NULL;
    bool is_update = strstr(line, "\"method\"") != NULL && strstr(line, "session/update") != NULL;
    if (is_permission) {
      wxa_acp_handle_permission_request(agent, line);
    } else if (is_update) {
      wxa_acp_handle_prompt_update(agent, line);
    } else {
      bool found_id = false;
      long response_id = wxa_acp_json_get_long(line, "id", &found_id);
      if (found_id && agent->pending.id == response_id && !agent->pending.done) {
        agent->pending.done = true;
        agent->pending.failed = wxa_acp_find_after_key(line, "error") != NULL;
        agent->pending.payload = sp_str_from_cstr(line);
        pthread_cond_broadcast(&agent->cond);
      }
    }
    pthread_mutex_unlock(&agent->mutex);
  }

  pthread_mutex_lock(&agent->mutex);
  agent->ready = false;
  pthread_cond_broadcast(&agent->cond);
  pthread_mutex_unlock(&agent->mutex);
  free(line);
  return NULL;
}

static bool wxa_acp_wait_response(
  wxa_acp_agent_t* agent,
  long request_id,
  unsigned int timeout_ms,
  sp_str_t* out_payload
) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return false;
  }
  ts.tv_sec += (time_t)(timeout_ms / 1000U);
  ts.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&agent->mutex);
  agent->pending.id = request_id;
  agent->pending.done = false;
  agent->pending.failed = false;
  wxa_acp_free_str(&agent->pending.payload);

  while (!agent->pending.done && agent->ready) {
    int rc = pthread_cond_timedwait(&agent->cond, &agent->mutex, &ts);
    if (rc == ETIMEDOUT) {
      break;
    }
  }

  if (agent->pending.done && !agent->pending.failed) {
    if (out_payload != NULL) {
      *out_payload = sp_str_copy(agent->pending.payload);
    }
    pthread_mutex_unlock(&agent->mutex);
    return true;
  }

  if (agent->pending.done && agent->pending.failed) {
    wxa_acp_set_error(agent, "acp request failed");
  } else if (!agent->ready) {
    int status = 0;
    pid_t waited = -1;
    if (agent->child_pid > 0) {
      waited = waitpid(agent->child_pid, &status, WNOHANG);
    }
    if (waited == agent->child_pid && WIFEXITED(status)) {
      agent->child_pid = 0;
      int code = WEXITSTATUS(status);
      sp_str_t msg = wxa_acp_printf(
        "acp subprocess stopped: command='%s' exit_code=%d%s",
        agent->command.data != NULL ? agent->command.data : "",
        code,
        code == 127 ? " (command not found or not executable)" : ""
      );
      wxa_acp_set_error(agent, msg.data);
      wxa_acp_free_str(&msg);
    } else if (waited == agent->child_pid && WIFSIGNALED(status)) {
      agent->child_pid = 0;
      sp_str_t msg = wxa_acp_printf(
        "acp subprocess stopped: command='%s' signal=%d",
        agent->command.data != NULL ? agent->command.data : "",
        WTERMSIG(status)
      );
      wxa_acp_set_error(agent, msg.data);
      wxa_acp_free_str(&msg);
    } else {
      wxa_acp_set_error(agent, "acp subprocess stopped");
    }
  } else {
    wxa_acp_set_error(agent, "acp request timed out");
  }
  pthread_mutex_unlock(&agent->mutex);
  return false;
}

static bool wxa_acp_send_request(
  wxa_acp_agent_t* agent,
  const char* method,
  sp_str_t params_json,
  unsigned int timeout_ms,
  sp_str_t* out_payload
) {
  long request_id;
  sp_str_t payload;
  pthread_mutex_lock(&agent->mutex);
  request_id = agent->pending.id + 1;
  agent->pending.id = request_id;
  pthread_mutex_unlock(&agent->mutex);

  payload = wxa_acp_printf(
    "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":\"%s\",\"params\":%s}",
    request_id,
    method,
    params_json.data
  );
  if (!wxa_acp_write_line(agent, payload)) {
    wxa_acp_set_error(agent, "failed to write acp request");
    wxa_acp_free_str(&payload);
    return false;
  }
  wxa_acp_free_str(&payload);
  return wxa_acp_wait_response(agent, request_id, timeout_ms, out_payload);
}

static bool wxa_acp_start_process(wxa_acp_agent_t* agent) {
  int stdin_pipe[2];
  int stdout_pipe[2];
  pid_t pid;
  char** argv;

  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
    wxa_acp_set_error(agent, "failed to create pipes");
    return false;
  }

  pid = fork();
  if (pid < 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    wxa_acp_set_error(agent, "failed to fork acp process");
    return false;
  }

  if (pid == 0) {
    int devnull_fd = -1;
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    devnull_fd = open("/dev/null", O_WRONLY);
    if (devnull_fd >= 0) {
      dup2(devnull_fd, STDERR_FILENO);
      close(devnull_fd);
    }
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    if (agent->cwd.len > 0U) {
      char* cwd = sp_str_to_cstr(agent->cwd);
      if (chdir(cwd) != 0) {
      }
      sp_free(cwd);
    }
    argv = sp_alloc_n(char*, (u32)sp_dyn_array_size(agent->args) + 2U);
    argv[0] = sp_str_to_cstr(agent->command);
    sp_dyn_array_for(agent->args, i) {
      argv[i + 1U] = sp_str_to_cstr(agent->args[i]);
    }
    argv[sp_dyn_array_size(agent->args) + 1U] = NULL;
    execvp(argv[0], argv);
    _exit(127);
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  agent->stdin_fd = stdin_pipe[1];
  agent->stdout_fd = stdout_pipe[0];
  agent->stdout_file = fdopen(agent->stdout_fd, "r");
  agent->child_pid = pid;
  if (agent->stdout_file == NULL) {
    close(agent->stdin_fd);
    close(agent->stdout_fd);
    wxa_acp_set_error(agent, "failed to open acp stdout");
    return false;
  }

  agent->ready = true;
  agent->stop_reader = false;
  if (pthread_create(&agent->reader_thread, NULL, wxa_acp_reader_main, agent) != 0) {
    fclose(agent->stdout_file);
    close(agent->stdin_fd);
    agent->stdout_file = NULL;
    agent->stdin_fd = -1;
    agent->ready = false;
    wxa_acp_set_error(agent, "failed to start acp reader thread");
    return false;
  }
  return true;
}

static bool wxa_acp_initialize(wxa_acp_agent_t* agent) {
  sp_str_t params = sp_str_lit("");
  sp_str_t response = sp_str_lit("");
  params = wxa_acp_printf(
    "{\"protocolVersion\":%d,\"clientCapabilities\":{},\"clientInfo\":{\"name\":\"weixin-agent-c-sdk\",\"version\":\"0.1.0\"}}",
    WXA_ACP_PROTOCOL_VERSION
  );
  if (!wxa_acp_send_request(agent, "initialize", params, 10000U, &response)) {
    wxa_acp_free_str(&params);
    return false;
  }
  agent->capabilities.image = wxa_acp_json_has_true(response.data, "image");
  agent->capabilities.audio = wxa_acp_json_has_true(response.data, "audio");
  agent->capabilities.embedded_context = wxa_acp_json_has_true(response.data, "embeddedContext");
  agent->capabilities.load_session = wxa_acp_json_has_true(response.data, "loadSession");
  wxa_acp_free_str(&response);
  wxa_acp_free_str(&params);
  return true;
}

static bool wxa_acp_ensure_ready(wxa_acp_agent_t* agent) {
  if (agent == NULL) {
    return false;
  }
  if (agent->ready) {
    return true;
  }
  if (!wxa_acp_start_process(agent)) {
    return false;
  }
  if (!wxa_acp_initialize(agent)) {
    return false;
  }
  return true;
}

static char* wxa_acp_realpath_dup(sp_str_t path) {
  char cwd[PATH_MAX];
  char* cpath = sp_str_to_cstr(path);
  char* resolved;
  if (cpath[0] == '/') {
    return cpath;
  }
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    return cpath;
  }
  resolved = sp_str_to_cstr(sp_format("{}/{}", SP_FMT_CSTR(cwd), SP_FMT_CSTR(cpath)));
  sp_free(cpath);
  return resolved;
}

static sp_str_t wxa_acp_build_prompt_json(wxa_acp_agent_t* agent, const wxa_chat_request_t* request) {
  (void)agent;
  sp_str_builder_t builder = SP_ZERO_INITIALIZE();
  bool first = true;
  sp_str_builder_append_cstr(&builder, "[");

  if (request->text != NULL && request->text[0] != '\0') {
    sp_str_t text = wxa_acp_json_escape(request->text);
    sp_str_t block = wxa_acp_printf("{\"type\":\"text\",\"text\":\"%s\"}", text.data);
    sp_str_builder_append_cstr(&builder, block.data);
    first = false;
    wxa_acp_free_str(&block);
    wxa_acp_free_str(&text);
  }

  if (request->media.type != WXA_MEDIA_NONE && request->media.file_path != NULL) {
    sp_str_t mime = sp_str_view(request->media.mime_type != NULL ? request->media.mime_type : "application/octet-stream");
    sp_str_t b64 = wxa_acp_read_file_base64(request->media.file_path);
    char* abs_path = wxa_acp_realpath_dup(sp_str_view(request->media.file_path));
    sp_str_t uri = abs_path != NULL ? wxa_acp_printf("file://%s", abs_path) : sp_str_lit("");
    sp_str_t escaped_uri = wxa_acp_json_escape(uri.data);
    sp_str_t escaped_mime = wxa_acp_json_escape(mime.data);
    sp_str_t block = sp_str_lit("");
    if (!first) {
      sp_str_builder_append_cstr(&builder, ",");
    }
    if (request->media.type == WXA_MEDIA_IMAGE) {
      block = wxa_acp_printf(
        "{\"type\":\"image\",\"mimeType\":\"%s\",\"data\":\"%s\"}",
        escaped_mime.data,
        b64.data
      );
    } else if (request->media.type == WXA_MEDIA_AUDIO) {
      block = wxa_acp_printf(
        "{\"type\":\"audio\",\"mimeType\":\"%s\",\"data\":\"%s\"}",
        escaped_mime.data,
        b64.data
      );
    } else {
      block = wxa_acp_printf(
        "{\"type\":\"resource\",\"resource\":{\"uri\":\"%s\",\"mimeType\":\"%s\",\"blob\":\"%s\"}}",
        escaped_uri.data,
        escaped_mime.data,
        b64.data
      );
    }
    sp_str_builder_append_cstr(&builder, block.data);
    wxa_acp_free_str(&block);
    wxa_acp_free_str(&escaped_mime);
    wxa_acp_free_str(&escaped_uri);
    wxa_acp_free_str(&uri);
    wxa_acp_free_str(&b64);
    if (abs_path != NULL) {
      sp_free(abs_path);
    }
  }

  sp_str_builder_append_cstr(&builder, "]");
  return sp_str_builder_to_str(&builder);
}

static sp_str_t wxa_acp_session_for_conversation(wxa_acp_agent_t* agent, sp_str_t conversation_id) {
  sp_dyn_array_for(agent->sessions, i) {
    if (sp_str_equal(agent->sessions[i].conversation_id, conversation_id)) {
      return sp_str_copy(agent->sessions[i].session_id);
    }
  }
  return sp_str_lit("");
}

static bool wxa_acp_create_session(wxa_acp_agent_t* agent, sp_str_t conversation_id, sp_str_t* out_session_id) {
  char cwd_buf[PATH_MAX];
  sp_str_t cwd = sp_str_lit("");
  sp_str_t params = sp_str_lit("");
  sp_str_t response = sp_str_lit("");
  wxa_acp_session_entry_t entry = {0};

  if (agent->cwd.len > 0U) {
    cwd = sp_str_copy(agent->cwd);
  } else if (getcwd(cwd_buf, sizeof(cwd_buf)) != NULL) {
    cwd = sp_str_from_cstr(cwd_buf);
  } else {
    wxa_acp_set_error(agent, "failed to resolve cwd");
    return false;
  }

  params = wxa_acp_printf("{\"cwd\":\"%s\",\"mcpServers\":[]}", cwd.data);
  if (!wxa_acp_send_request(agent, "session/new", params, 10000U, &response)) {
    wxa_acp_free_str(&cwd);
    wxa_acp_free_str(&params);
    return false;
  }

  *out_session_id = wxa_acp_json_get_string(response.data, "sessionId");
  if (out_session_id->len == 0U) {
    wxa_acp_set_error(agent, "acp session/new missing sessionId");
    wxa_acp_free_str(&cwd);
    wxa_acp_free_str(&params);
    wxa_acp_free_str(&response);
    return false;
  }

  entry.conversation_id = sp_str_copy(conversation_id);
  entry.session_id = sp_str_copy(*out_session_id);
  sp_dyn_array_push(agent->sessions, entry);

  wxa_acp_free_str(&cwd);
  wxa_acp_free_str(&params);
  wxa_acp_free_str(&response);
  return true;
}

wxa_acp_agent_t* wxa_acp_agent_new(const wxa_acp_agent_options_t* options) {
  wxa_acp_agent_t* agent = sp_alloc(sizeof(*agent));
  if (agent == NULL) {
    return NULL;
  }
  *agent = (wxa_acp_agent_t){
    .stdin_fd = -1,
    .stdout_fd = -1,
    .child_pid = -1,
    .prompt_timeout_ms = options != NULL && options->prompt_timeout_ms > 0U
      ? options->prompt_timeout_ms
      : WXA_ACP_DEFAULT_PROMPT_TIMEOUT_MS
  };
  agent->command = sp_str_from_cstr(options != NULL && options->command != NULL ? options->command : "");
  agent->cwd = sp_str_from_cstr(options != NULL && options->cwd != NULL ? options->cwd : "");
  if (options != NULL && options->args != NULL) {
    for (int i = 0; i < options->arg_count; ++i) {
      sp_dyn_array_push(agent->args, sp_str_from_cstr(options->args[i]));
    }
  }
  pthread_mutex_init(&agent->mutex, NULL);
  pthread_cond_init(&agent->cond, NULL);
  return agent;
}

void wxa_acp_agent_free(wxa_acp_agent_t* agent) {
  if (agent == NULL) {
    return;
  }
  agent->stop_reader = true;
  if (agent->stdin_fd >= 0) {
    close(agent->stdin_fd);
  }
  if (agent->stdout_file != NULL) {
    fclose(agent->stdout_file);
  }
  if (agent->child_pid > 0) {
    kill(agent->child_pid, SIGTERM);
    (void)waitpid(agent->child_pid, NULL, 0);
  }
  if (agent->reader_thread != 0U) {
    pthread_join(agent->reader_thread, NULL);
  }
  sp_dyn_array_for(agent->args, i) {
    wxa_acp_free_str(&agent->args[i]);
  }
  sp_dyn_array_free(agent->args);
  sp_dyn_array_for(agent->sessions, i) {
    wxa_acp_free_str(&agent->sessions[i].conversation_id);
    wxa_acp_free_str(&agent->sessions[i].session_id);
  }
  sp_dyn_array_free(agent->sessions);
  wxa_acp_free_str(&agent->command);
  wxa_acp_free_str(&agent->cwd);
  wxa_acp_free_str(&agent->last_error);
  wxa_acp_free_str(&agent->active_session_id);
  wxa_acp_prompt_result_reset(&agent->prompt_result);
  wxa_acp_free_str(&agent->pending.payload);
  pthread_cond_destroy(&agent->cond);
  pthread_mutex_destroy(&agent->mutex);
  sp_free(agent);
}

int wxa_acp_agent_chat(
  wxa_acp_agent_t* agent,
  const wxa_chat_request_t* request,
  wxa_chat_response_t* response
) {
  sp_str_t conversation_id = sp_str_lit("");
  sp_str_t session_id = sp_str_lit("");
  sp_str_t prompt_blocks = sp_str_lit("");
  sp_str_t params = sp_str_lit("");
  sp_str_t prompt_response = sp_str_lit("");

  if (agent == NULL || request == NULL || response == NULL || agent->command.len == 0U) {
    return -1;
  }
  if (!wxa_acp_ensure_ready(agent)) {
    return -1;
  }

  conversation_id = sp_str_from_cstr(request->conversation_id != NULL ? request->conversation_id : "");
  session_id = wxa_acp_session_for_conversation(agent, conversation_id);
  if (session_id.len == 0U && !wxa_acp_create_session(agent, conversation_id, &session_id)) {
    wxa_acp_free_str(&conversation_id);
    return -1;
  }

  prompt_blocks = wxa_acp_build_prompt_json(agent, request);
  params = wxa_acp_printf(
    "{\"sessionId\":\"%s\",\"prompt\":%s}",
    session_id.data,
    prompt_blocks.data
  );

  pthread_mutex_lock(&agent->mutex);
  agent->prompt_active = true;
  wxa_acp_free_str(&agent->active_session_id);
  agent->active_session_id = sp_str_copy(session_id);
  wxa_acp_prompt_result_reset(&agent->prompt_result);
  pthread_mutex_unlock(&agent->mutex);

  if (!wxa_acp_send_request(agent, "session/prompt", params, agent->prompt_timeout_ms, &prompt_response)) {
    pthread_mutex_lock(&agent->mutex);
    agent->prompt_active = false;
    pthread_mutex_unlock(&agent->mutex);
    wxa_acp_free_str(&conversation_id);
    wxa_acp_free_str(&session_id);
    wxa_acp_free_str(&prompt_blocks);
    wxa_acp_free_str(&params);
    return -1;
  }

  pthread_mutex_lock(&agent->mutex);
  agent->prompt_active = false;
  if (agent->prompt_result.text.len > 0U) {
    response->text = sp_str_to_cstr(agent->prompt_result.text);
  }
  if (agent->prompt_result.image_base64.len > 0U) {
    sp_str_t file_path = wxa_acp_write_image_file(
      agent->prompt_result.image_base64,
      agent->prompt_result.image_mime_type.len > 0U ? agent->prompt_result.image_mime_type : sp_str_lit("image/png")
    );
    if (file_path.len > 0U) {
      response->media.type = WXA_MEDIA_IMAGE;
      response->media.file_path = sp_str_to_cstr(file_path);
      response->media.mime_type = "image/png";
      response->media.file_name = NULL;
      wxa_acp_free_str(&file_path);
    }
  }
  pthread_mutex_unlock(&agent->mutex);

  wxa_acp_free_str(&conversation_id);
  wxa_acp_free_str(&session_id);
  wxa_acp_free_str(&prompt_blocks);
  wxa_acp_free_str(&params);
  wxa_acp_free_str(&prompt_response);
  return 0;
}

const char* wxa_acp_agent_last_error(const wxa_acp_agent_t* agent) {
  if (agent == NULL || agent->last_error.data == NULL) {
    return "";
  }
  return agent->last_error.data;
}
