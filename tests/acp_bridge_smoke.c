#include "weixin_acp_bridge.h"

#include <stdio.h>
#include <string.h>

static int expect(int condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "acp smoke failed: %s\n", message);
    return 1;
  }
  return 0;
}

int main(void) {
  const char* args[] = { "tests/mock_acp_agent.py" };
  wxa_acp_agent_t* agent = wxa_acp_agent_new(&(wxa_acp_agent_options_t){
    .command = "python3",
    .args = args,
    .arg_count = 1,
    .cwd = NULL,
    .prompt_timeout_ms = 5000U
  });
  wxa_chat_request_t request = {
    .conversation_id = "conv-1",
    .text = "hello bridge",
    .media = {0}
  };
  wxa_chat_response_t response = {0};
  int rc = 0;

  if (agent == NULL) {
    fprintf(stderr, "acp smoke failed: no agent\n");
    return 1;
  }
  if (wxa_acp_agent_chat(agent, &request, &response) != 0) {
    fprintf(stderr, "acp smoke failed: %s\n", wxa_acp_agent_last_error(agent));
    wxa_acp_agent_free(agent);
    return 1;
  }

  rc |= expect(response.text != NULL, "missing text response");
  rc |= expect(response.text != NULL && strcmp(response.text, "mock:hello bridge") == 0, "unexpected text response");
  wxa_acp_agent_free(agent);
  return rc;
}
