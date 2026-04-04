/*
 * test_config.c — Unit tests for the INI config parser (config.c)
 *
 * Each test function:
 *   1. Writes a small INI file to a temp path
 *   2. Calls config_load()
 *   3. Asserts the expected values were parsed correctly
 *
 * Why write to a real temp file instead of a string buffer?
 * Because config_load() takes a file path. Testing with real files means
 * we test the actual code path including fopen/fgets — not a mocked version.
 * Real files = real tests.
 */

#include "test_framework.h"
#include "../src/config.h"
#include "../src/service.h"

#include <stdio.h>    /* FILE, fopen, fclose, fprintf, tmpnam, remove */
#include <string.h>   /* strcmp */
#include <stdlib.h>   /* EXIT_SUCCESS, EXIT_FAILURE */

/* ==========================================================================
 * Helper: write a string to a temp file, return the path.
 * The caller is responsible for calling remove(path) and free(path).
 *
 * tmpnam() generates a unique temporary filename.
 * It's not the most secure function (use mkstemp on Linux in production),
 * but it's cross-platform and fine for tests.
 * ==========================================================================
 */
static char *write_temp_config(const char *content) {
    static char path[L_tmpnam];
    tmpnam(path);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "test_config: could not create temp file\n");
        return NULL;
    }
    fprintf(f, "%s", content);
    fclose(f);
    return path;
}

/* ==========================================================================
 * Test cases
 * ==========================================================================
 */

static void test_single_service_basic(void) {
    TEST("parse a single service with required fields");

    const char *ini =
        "[supervisor]\n"
        "ipc_socket = /tmp/guardian.sock\n"
        "\n"
        "[my-api]\n"
        "command = /usr/bin/node\n"
        "args    = server.js\n"
        "restart = on_failure\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    ASSERT_INT_EQ(1, ctx.service_count);
    ASSERT_STR_EQ("my-api", ctx.services[0].cfg.name);
    ASSERT_STR_EQ("/usr/bin/node", ctx.services[0].cfg.command);
    ASSERT_STR_EQ("server.js", ctx.services[0].cfg.args);
    ASSERT_INT_EQ((int)RESTART_ON_FAILURE, (int)ctx.services[0].cfg.restart_policy);
}

static void test_supervisor_section(void) {
    TEST("parse [supervisor] section fields");

    const char *ini =
        "[supervisor]\n"
        "log_file   = /var/log/guardian.log\n"
        "ipc_socket = /tmp/guardian.sock\n"
        "\n"
        "[worker]\n"
        "command = /opt/worker\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    ASSERT_STR_EQ("/var/log/guardian.log", ctx.log_file);
    ASSERT_STR_EQ("/tmp/guardian.sock", ctx.ipc_socket);
}

static void test_multiple_services(void) {
    TEST("parse three services into separate slots");

    const char *ini =
        "[svc-a]\n"
        "command = /bin/a\n"
        "\n"
        "[svc-b]\n"
        "command = /bin/b\n"
        "\n"
        "[svc-c]\n"
        "command = /bin/c\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    ASSERT_INT_EQ(3, ctx.service_count);
    ASSERT_STR_EQ("svc-a", ctx.services[0].cfg.name);
    ASSERT_STR_EQ("svc-b", ctx.services[1].cfg.name);
    ASSERT_STR_EQ("svc-c", ctx.services[2].cfg.name);
    ASSERT_STR_EQ("/bin/a", ctx.services[0].cfg.command);
    ASSERT_STR_EQ("/bin/b", ctx.services[1].cfg.command);
    ASSERT_STR_EQ("/bin/c", ctx.services[2].cfg.command);
}

static void test_health_check_tcp(void) {
    TEST("parse a TCP health check and attach it to the right service");

    const char *ini =
        "[api]\n"
        "command = /bin/api\n"
        "\n"
        "[health_check]\n"
        "type                = tcp\n"
        "target              = 127.0.0.1:8080\n"
        "interval_s          = 10\n"
        "timeout_s           = 3\n"
        "unhealthy_threshold = 2\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    ASSERT_INT_EQ(1, ctx.services[0].cfg.has_health_check);
    ASSERT_INT_EQ((int)HEALTH_TCP, (int)ctx.services[0].cfg.health.type);
    ASSERT_STR_EQ("127.0.0.1:8080", ctx.services[0].cfg.health.target);
    ASSERT_INT_EQ(10, ctx.services[0].cfg.health.interval_s);
    ASSERT_INT_EQ(3,  ctx.services[0].cfg.health.timeout_s);
    ASSERT_INT_EQ(2,  ctx.services[0].cfg.health.unhealthy_threshold);
}

static void test_health_check_http(void) {
    TEST("parse an HTTP health check");

    const char *ini =
        "[web]\n"
        "command = /bin/web\n"
        "\n"
        "[health_check]\n"
        "type   = http\n"
        "target = http://127.0.0.1:3000/health\n"
        "interval_s = 5\n"
        "timeout_s  = 2\n"
        "unhealthy_threshold = 3\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    ASSERT_INT_EQ((int)HEALTH_HTTP, (int)ctx.services[0].cfg.health.type);
    ASSERT_STR_EQ("http://127.0.0.1:3000/health", ctx.services[0].cfg.health.target);
}

static void test_restart_policies(void) {
    TEST("parse all three restart policy values");

    /* We test each value by loading a config with just that service */
    const char *never_ini =
        "[s]\ncommand = /bin/x\nrestart = never\n";
    const char *always_ini =
        "[s]\ncommand = /bin/x\nrestart = always\n";
    const char *failure_ini =
        "[s]\ncommand = /bin/x\nrestart = on_failure\n";

    char *p; SupervisorContext ctx;

    p = write_temp_config(never_ini);
    config_load(p, &ctx); remove(p);
    ASSERT_INT_EQ((int)RESTART_NEVER, (int)ctx.services[0].cfg.restart_policy);

    TEST("restart = always parses correctly");
    p = write_temp_config(always_ini);
    config_load(p, &ctx); remove(p);
    ASSERT_INT_EQ((int)RESTART_ALWAYS, (int)ctx.services[0].cfg.restart_policy);

    TEST("restart = on_failure parses correctly");
    p = write_temp_config(failure_ini);
    config_load(p, &ctx); remove(p);
    ASSERT_INT_EQ((int)RESTART_ON_FAILURE, (int)ctx.services[0].cfg.restart_policy);
}

static void test_backoff_values(void) {
    TEST("parse backoff_base_ms and backoff_max_ms");

    const char *ini =
        "[svc]\n"
        "command         = /bin/x\n"
        "backoff_base_ms = 500\n"
        "backoff_max_ms  = 30000\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    ASSERT_INT_EQ(500,   ctx.services[0].cfg.backoff_base_ms);
    ASSERT_INT_EQ(30000, ctx.services[0].cfg.backoff_max_ms);
}

static void test_max_retries_zero_means_unlimited(void) {
    TEST("max_retries = 0 is stored as 0 (sentinel for unlimited)");

    const char *ini =
        "[svc]\n"
        "command     = /bin/x\n"
        "max_retries = 0\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    ASSERT_INT_EQ(0, ctx.services[0].cfg.max_retries);
}

static void test_inline_comments_stripped(void) {
    TEST("inline comments after values are stripped");

    const char *ini =
        "[svc]\n"
        "command    = /bin/myapp  ; the main application\n"
        "max_retries = 3          ; try 3 times\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    ASSERT_STR_EQ("/bin/myapp", ctx.services[0].cfg.command);
    ASSERT_INT_EQ(3, ctx.services[0].cfg.max_retries);
}

static void test_blank_lines_and_comments_ignored(void) {
    TEST("blank lines and comment-only lines are skipped without error");

    const char *ini =
        "; This is a full-line comment\n"
        "# This is also a comment\n"
        "\n"
        "   \n"               /* whitespace-only line */
        "[svc]\n"
        "; another comment inside a section\n"
        "command = /bin/x\n"
        "\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    ASSERT_INT_EQ(1, ctx.service_count);
    ASSERT_STR_EQ("/bin/x", ctx.services[0].cfg.command);
}

static void test_missing_command_is_error(void) {
    TEST("service without 'command' key returns CONFIG_ERR_NO_COMMAND");

    const char *ini =
        "[svc]\n"
        "restart = always\n";  /* no command key! */

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_ERR_NO_COMMAND, (int)err);
}

static void test_nonexistent_file_is_error(void) {
    TEST("nonexistent config file returns CONFIG_ERR_FILE");

    SupervisorContext ctx;
    ConfigError err = config_load("/this/path/does/not/exist.ini", &ctx);

    ASSERT_INT_EQ(CONFIG_ERR_FILE, (int)err);
}

static void test_invalid_restart_value_is_error(void) {
    TEST("unrecognized restart value returns CONFIG_ERR_SYNTAX");

    const char *ini =
        "[svc]\n"
        "command = /bin/x\n"
        "restart = sometimes\n";  /* invalid value */

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_ERR_SYNTAX, (int)err);
}

static void test_health_check_before_service_is_error(void) {
    TEST("[health_check] before any service section returns CONFIG_ERR_SYNTAX");

    const char *ini =
        "[supervisor]\n"
        "ipc_socket = /tmp/x.sock\n"
        "\n"
        "[health_check]\n"   /* no service above this! */
        "type   = tcp\n"
        "target = 127.0.0.1:8080\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_ERR_SYNTAX, (int)err);
}

static void test_unknown_keys_ignored(void) {
    TEST("unknown keys in a service section are silently ignored");

    const char *ini =
        "[svc]\n"
        "command          = /bin/x\n"
        "future_feature   = some_value\n"  /* unknown key */
        "another_unknown  = 42\n";         /* unknown key */

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    /* Should succeed — unknown keys are ignored for forward-compatibility */
    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    ASSERT_STR_EQ("/bin/x", ctx.services[0].cfg.command);
}

static void test_autostart_values(void) {
    TEST("autostart = true and autostart = false parse correctly");

    const char *true_ini  = "[s]\ncommand=/bin/x\nautostart=true\n";
    const char *false_ini = "[s]\ncommand=/bin/x\nautostart=false\n";
    char *p; SupervisorContext ctx;

    p = write_temp_config(true_ini);
    config_load(p, &ctx); remove(p);
    ASSERT_INT_EQ(1, ctx.services[0].cfg.autostart);

    TEST("autostart = false sets field to 0");
    p = write_temp_config(false_ini);
    config_load(p, &ctx); remove(p);
    ASSERT_INT_EQ(0, ctx.services[0].cfg.autostart);
}

static void test_default_values_applied(void) {
    TEST("services get sensible defaults when optional keys are absent");

    const char *ini = "[svc]\ncommand = /bin/x\n";

    char *path = write_temp_config(ini);
    SupervisorContext ctx;
    ConfigError err = config_load(path, &ctx);
    remove(path);

    ASSERT_INT_EQ(CONFIG_OK, (int)err);
    /* Defaults set in config.c */
    ASSERT_INT_EQ(1,    ctx.services[0].cfg.autostart);
    ASSERT_INT_EQ(5,    ctx.services[0].cfg.max_retries);
    ASSERT_INT_EQ(1000, ctx.services[0].cfg.backoff_base_ms);
    ASSERT_INT_EQ(60000,ctx.services[0].cfg.backoff_max_ms);
    ASSERT_INT_EQ((int)RESTART_ON_FAILURE,
                  (int)ctx.services[0].cfg.restart_policy);
}

/* ==========================================================================
 * Test runner
 * ==========================================================================
 */

int main(void) {
    printf("\n  guardian — config parser tests\n");
    printf("  ================================\n\n");

    test_single_service_basic();
    test_supervisor_section();
    test_multiple_services();
    test_health_check_tcp();
    test_health_check_http();
    test_restart_policies();
    test_backoff_values();
    test_max_retries_zero_means_unlimited();
    test_inline_comments_stripped();
    test_blank_lines_and_comments_ignored();
    test_missing_command_is_error();
    test_nonexistent_file_is_error();
    test_invalid_restart_value_is_error();
    test_health_check_before_service_is_error();
    test_unknown_keys_ignored();
    test_autostart_values();
    test_default_values_applied();

    SUMMARY();
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
