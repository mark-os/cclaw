/* T253: channel_emit → daemon_wake → agent fork → outbox delivery cycle.
 * Uses a mock channel binary that emits one message and reads outbox. */
#define _GNU_SOURCE
#include "daemon.h"
#include "db.h"
#include "config.h"
#include "secret.h"
#include "mock_server.h"
#include "channel_api.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_chan_e2e/cclaw.db"
#define WORK_DIR "/tmp/test_chan_e2e"
#define AGENT_DB WORK_DIR "/agents/default/agent.db"
#define MOCK_CHAN "/tmp/test_chan_e2e/mock_chan.sh"

static int s_mock_port;

static void cleanup(void) {
    system("rm -rf /tmp/test_chan_e2e");
}

/* Create mock channel script that emits one message via sqlite, writes to FIFO,
 * then polls outbox until delivered or timeout */
static void create_mock_channel(void) {
    FILE *f = fopen(MOCK_CHAN, "w");
    assert(f);
    /* Args: $1=db_path, $2=channel_name */
    fprintf(f, "#!/bin/sh\n"
        "DB=$1\n"
        "CH=$2\n"
        "# Derive FIFO path (replace .db with .pipe)\n"
        "FIFO=$(echo \"$DB\" | sed 's/\\.db$/.pipe/')\n"
        "# Wait for daemon FIFO\n"
        "for i in $(seq 1 10); do [ -p \"$FIFO\" ] && break; sleep 0.2; done\n"
        "# Emit a message event\n"
        "sqlite3 \"$DB\" \"INSERT INTO channel_events(channel_name,event_type,payload) "
        "VALUES('$CH','message','{\\\"channel_id\\\":\\\"test\\\",\\\"text\\\":\\\"hello agent\\\"}');\"\n"
        "# Wake daemon via FIFO\n"
        "printf '\\x01' > \"$FIFO\"\n"
        "# Poll outbox for our delivery (max 10s)\n"
        "for i in $(seq 1 20); do\n"
        "  ROW=$(sqlite3 \"$DB\" \"SELECT payload FROM channel_outbox WHERE channel_name='$CH' AND status='pending' LIMIT 1;\")\n"
        "  if [ -n \"$ROW\" ]; then\n"
        "    # Ack it\n"
        "    sqlite3 \"$DB\" \"UPDATE channel_outbox SET status='delivered', acked_at=unixepoch() WHERE channel_name='$CH' AND status='pending';\"\n"
        "    echo \"DELIVERED:$ROW\"\n"
        "    exit 0\n"
        "  fi\n"
        "  sleep 0.5\n"
        "done\n"
        "echo \"TIMEOUT\"\n"
        "exit 1\n");
    fclose(f);
    chmod(MOCK_CHAN, 0755);
}

static void test_channel_e2e(void) {
    cleanup();
    system("mkdir -p " WORK_DIR);
    create_mock_channel();

    /* Set up mock LLM server */
    s_mock_port = mock_server_start();
    mock_server_enqueue(200,
        "{\"id\":\"x\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"I received your message.\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}");

    /* Create daemon DB */
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    uint8_t key[32];
    secret_key_load_or_create(DB_PATH, key);
    db_set_secret_key(key);

    /* Register mock channel */
    sqlite3_exec(db, "INSERT INTO channels(name,type,binary_path) "
                     "VALUES('mock','mock','" MOCK_CHAN "');", NULL, NULL, NULL);

    /* Bind channel to agent */
    db_channel_binding_set(db, "mock", "default", "default");

    /* Create agent directory + DB */
    system("mkdir -p " WORK_DIR "/agents/default/workspace");
    sqlite3 *adb = db_open(AGENT_DB);
    assert(adb);
    db_close(adb);

    /* Register agent in daemon DB */
    sqlite3_exec(db, "INSERT OR IGNORE INTO agents(name,config) "
                     "VALUES('default','{}');", NULL, NULL, NULL);

    /* Set provider config pointing to mock */
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_mock_port);
    db_kv_set(db, "provider.base_url", url);
    db_kv_set(db, "provider.api_key", "test-key");
    db_kv_set(db, "provider.model", "test-model");

    db_close(db);

    /* Set env for agent discovery */
    setenv("CCLAW_DB_PATH", DB_PATH, 1);

    /* Run daemon in child */
    Config cfg = {0};
    cfg.db_path = DB_PATH;

    /* Resolve cclaw binary path (before fork changes CWD) */
    char cclaw_bin[1024];
    {
        char *rp = realpath("./build/cclaw", NULL);
        assert(rp && "build/cclaw must exist — run make first");
        snprintf(cclaw_bin, sizeof(cclaw_bin), "%s", rp);
        free(rp);
    }

    pid_t dpid = fork();
    assert(dpid >= 0);
    if (dpid == 0) {
        /* Daemon must find agents/ relative to CWD */
        chdir(WORK_DIR);
        /* Point daemon at the real cclaw binary for agent fork+exec */
        daemon_set_self_path(cclaw_bin);
        sqlite3 *ddb = db_open(DB_PATH);
        daemon_run(&cfg, ddb);
        db_close(ddb);
        _exit(0);
    }

    /* Wait for channel to deliver or timeout */
    sleep(8);
    kill(dpid, SIGTERM);
    int status;
    waitpid(dpid, &status, 0);

    /* Verify outbox was delivered */
    db = db_open(DB_PATH);
    assert(db);
    sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_FULL, NULL, NULL);

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT status FROM channel_outbox WHERE channel_name='mock';",
        -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *st = (const char *)sqlite3_column_text(stmt, 0);
        assert(st != NULL);
        assert(strcmp(st, "delivered") == 0);
        sqlite3_finalize(stmt);
    } else {
        sqlite3_finalize(stmt);
        /* Check if agent even ran — look for entries in agent DB */
        adb = db_open(AGENT_DB);
        if (adb) {
            sqlite3_wal_checkpoint_v2(adb, NULL, SQLITE_CHECKPOINT_FULL, NULL, NULL);
            int cnt = 0;
            sqlite3_stmt *es;
            if (sqlite3_prepare_v2(adb, "SELECT COUNT(*) FROM entries;", -1, &es, NULL) == SQLITE_OK) {
                if (sqlite3_step(es) == SQLITE_ROW) cnt = sqlite3_column_int(es, 0);
                sqlite3_finalize(es);
            }
            fprintf(stderr, "agent entries: %d\n", cnt);
            db_close(adb);
        }
        assert(0 && "no outbox row found — channel delivery failed");
    }

    db_close(db);
    mock_server_stop();
    cleanup();
    printf("  PASS: test_channel_e2e\n");
}

int main(void) {
    printf("test_integration_channel_e2e (T253, V100, V101):\n");
    test_channel_e2e();
    printf("All channel e2e tests passed.\n");
    return 0;
}
