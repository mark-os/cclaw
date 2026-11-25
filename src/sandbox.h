#ifndef CCLAW_SANDBOX_H
#define CCLAW_SANDBOX_H

/* Containment policy and child setup (specs/containment.md). A containment is
 * a row of levels; the parent turns it into a plan — the enforcer chosen for
 * each level on this host, for one vehicle — or refuses the call. The child
 * applies the plan and never picks a weaker enforcer on its own. How a
 * boundary is built is the containment backend's job (contain.h). Shared by
 * every --run-tool child. Fail-closed by design. */

#include <limits.h>
#include <stddef.h>
#include <sqlite3.h>
#include "tools.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Levels, loosest first: a larger value is a stronger promise. */
typedef enum { FILES_OPEN, FILES_CONFINED, FILES_PRIVATE } FilesLevel;
typedef enum { NET_DIRECT, NET_ADVISORY, NET_FIREWALLED, NET_NONE } NetLevel;

/* Enforcers. FE_CODE: the path check in cclaw's own file and js code.
 * FE_LANDLOCK: a Landlock ruleset (for file and js, on top of the path
 * check). NE_ENV: the proxy variables only. NE_CODE: cclaw's curl dials the
 * proxy socket itself. NE_NETNS: an empty network namespace (with the mount
 * namespace, or alone). NE_SECCOMP: a filter refusing socket(). NE_NOTIF: a
 * filter that hands every connect() to the broker, which tunnels it through
 * the proxy (no namespace). Wire values: append only. */
typedef enum { FE_NONE, FE_CODE, FE_MOUNTNS, FE_LANDLOCK } FilesEnforcer;
typedef enum { NE_NONE, NE_ENV, NE_CODE, NE_NETNS, NE_SECCOMP, NE_NOTIF } NetEnforcer;

/* One containments row. Limits: 0 = no cap. */
typedef struct {
    FilesLevel files;
    NetLevel net;
    int nproc, cpu_sec;
} ContainLevels;

/* The plan for one child: the levels it promises and how each is kept. It is
 * what crosses the --run-tool wire. */
typedef struct {
    ContainLevels lv;
    FilesEnforcer fe;
    NetEnforcer ne;
    int transparent;  /* NE_NETNS, shell: connect() is tunnelled through the
                       * proxy too, and with the mount namespace a DNS stub
                       * answers names. A convenience: the namespace keeps the
                       * level either way. */
} ContainPlan;

/* The shipped rows, re-seeded at every start (db_seed_defaults). */
typedef struct { const char *name; ContainLevels lv; } ContainRow;
extern const ContainRow CONTAIN_SHIPPED[];
extern const size_t CONTAIN_SHIPPED_N;

/* An agent's containment, per tool context. Filled per dispatch batch in
 * agent_setup (containment_load; paths from the agent's grants). known=0: the
 * agent names a containment with no row, and every child call is refused.
 * Path pointers borrow AgentCaps and are rebound on cap refresh. */
typedef struct {
    char name[64];
    int known;
    ContainLevels lv;
    char **read_paths;  size_t read_path_count;    /* path grants */
    char **write_paths; size_t write_path_count;
} Containment;

/* A requested extra path (read_path/write_path grant). path is borrowed. */
typedef struct { const char *path; int ro; } SandboxMountReq;

/* A planned path: canonical path + ro flag, owned inline. */
typedef struct { char path[PATH_MAX]; int ro; } SandboxMount;

/* Child setup input, shared by every --run-tool child. All fields default to
 * 0/NULL. */
typedef struct {
    const char *workspace;  /* agent workspace, rw */
    const char *cwd_path;   /* the CLI's CWD; only sent under files 'open' */
    const char *db_path;    /* cclaw.db path: its dir holds .cclaw_key. Path only —
                               never opened; used to keep both out of reach. */
    const char *proxy_sock; /* the broker's proxy socket (NULL = none serving) */
    const char *llm_sock;   /* parent's LLM bridge UDS (NULL = no LLM()) */
    const char *tmp_dir;    /* the agent's persistent scratch dir, created and
                             * owned by the parent: /tmp in the mount namespace,
                             * TMPDIR otherwise. NULL = none. */
    ContainPlan plan;
    int skip_pid_ns;        /* 1 = omit CLONE_NEWPID (no inner fork); file, js */
    int notif_sock;         /* NE_NOTIF or transparent: the broker's end of a
                             * socketpair the filter's listener is sent on
                             * (0 = none) */
    SandboxMountReq *extra_mounts;   /* path grants */
    size_t extra_mount_count;
} SandboxConfig;

/* Run in the forked child before exec. Returns 0 on success, -1 on a hard
 * failure (caller must _exit 126). Applies exactly the plan; any enforcer that
 * cannot be applied is a failure, never a fallback. */
int sandbox_child_setup(const SandboxConfig *cfg);

/* The planner, pure: (levels, vehicle, host capabilities) → plan, or -1 with
 * `why` naming the containment, the level, what the host lacks and a shipped
 * containment it can keep. vehicle is EXEC_FILE, EXEC_JS or EXEC_SHELL. */
int contain_plan(const Containment *c, ExecVehicle vehicle, unsigned caps,
                 ContainPlan *out, char *why, size_t cap);

/* Every child vehicle can be planned for c on this host. */
int contain_keepable(const Containment *c, unsigned caps);

/* First shipped containment (strictest first) every child vehicle can keep
 * with these capabilities, or NULL. */
const char *contain_suggest(unsigned caps);

/* Load the containments row `name` (NULL = 'standard', the column default)
 * into c's name/known/lv; the path fields are untouched. Returns c->known. */
int containment_load(sqlite3 *db, const char *name, Containment *c);

/* Text names for logs, doctor and messages. */
const char *files_level_name(FilesLevel l);
const char *net_level_name(NetLevel l);
const char *files_enforcer_name(FilesEnforcer e);
const char *net_enforcer_name(NetEnforcer e);

/* FE_CODE, FE_LANDLOCK: may this process open `path` (write = for writing)?
 * 0 when it may, or no path check is armed (FE_NONE, FE_MOUNTNS); -1 with
 * errno=EACCES otherwise. Armed by sandbox_child_setup; called by the file
 * tools and the js fs/http/LLM globals before every open, stat or list. */
int contain_path_check(const char *path, int write);

/* Resolve extra-mount requests into a bind plan: canonicalize each path (drop
 * those whose realpath() fails), dedup by canonical path (rw wins over ro — a
 * write grant implies read of the same subtree), and sort shallow→deep so a
 * more-specific child mount binds after its parent and shadows it. out must
 * hold at least n entries; returns the deduped count. Pure — unit-testable. */
size_t sandbox_plan_mounts(const SandboxMountReq *in, size_t n, SandboxMount *out);

/* System directories a contained child reads: bound read-only in the mount
 * namespace, readable under FE_CODE. NULL-terminated. (/proc is the backend's
 * own business: only the shell, with its PID namespace, gets one. Landlock
 * takes /dev node by node and /proc read-only: sandbox.c.) */
extern const char *const SANDBOX_SYS_DIRS[];

#endif
