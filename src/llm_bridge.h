#ifndef CCLAW_LLM_BRIDGE_H
#define CCLAW_LLM_BRIDGE_H

/* Per-call UDS server for the JS tier's LLM() global (llm-core.md): the
 * parent (daemon or CLI — whichever process dispatched the tool) listens on
 * <agent-dir>/.llm.<pid>.<seq>.sock for the life of one tool child and
 * answers each connection with llm_request(). Per-call socket identity is
 * what lets the parent stamp session/iteration on llm_responses without the
 * sandboxed child asserting anything. Sibling of the egress proxy socket;
 * the sandbox bind-mounts it to a fixed in-namespace path and exports
 * CCLAW_LLM_SOCK.
 *
 * Protocol: one request per connection. 4-byte LE length + JSON
 * ({"messages":[...],"opts":{...},"images":[{mime,data}]?}) in; 4-byte LE length + JSON
 * ({"ok":true,...} | {"ok":false,"error":...}) out. */

#include <stdint.h>

/* Sized for opts.images: providers accept about 20 MB of inline image data
 * per request, and base64 is 4/3 of the file. LLM_IMAGES_MAX is the file
 * bytes the child will read; the request cap is that, encoded, plus the
 * conversation. Both follow the provider's limit, not this machine's speed. */
#define LLM_IMAGES_MAX     (15u * 1024 * 1024)
#define LLM_BRIDGE_REQ_MAX (LLM_IMAGES_MAX / 3 * 4 + 1024 * 1024)

typedef struct LlmBridge LlmBridge;

/* Bind + serve. `source` labels llm_responses rows (e.g. "js:web_search",
 * "cron:digest"). Returns NULL on failure — the tool then runs with no
 * LLM() (the child's calls fail with a connect error, fail-closed). */
LlmBridge *llm_bridge_start(const char *agent_dir, const char *db_path,
                            const char *agent_name, const char *source,
                            int64_t session_id, int64_t iteration_id);

/* Socket path for the wire/sandbox. Valid until llm_bridge_stop. */
const char *llm_bridge_sock(const LlmBridge *b);

/* Stop the accept thread, close + unlink the socket, free. NULL-safe. */
void llm_bridge_stop(LlmBridge *b);

#endif
