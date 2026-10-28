#include "test_framework.h"
#include "../src/platform/platform.h"

#define PIPE_PATH "/tmp/guardian-unit-test.sock"

#ifdef _WIN32
static void test_pipe_recovers_after_early_hangup(void) {
    TEST("pipe accepts a new client after one hangs up early");
    
    int server = platform_ipc_listen(PIPE_PATH);
    ASSERT(server >= 0);
    platform_ipc_accept(server);
    int first_client = platform_ipc_connect(PIPE_PATH);
    ASSERT(first_client >= 0);
    platform_ipc_close(first_client);

    for (int i = 0; i < 3; i++) {
        int connection = platform_ipc_accept(server);
        if (connection >= 0) {
            platform_ipc_close(connection);
        }

    }

    int second_client = platform_ipc_connect(PIPE_PATH);
    ASSERT(second_client >= 0);
    if (second_client >= 0) {
        platform_ipc_close(second_client);
    }

    platform_ipc_close(server);
}
#endif

int main(void) {
#ifdef _WIN32
    test_pipe_recovers_after_early_hangup();
#else
    printf("  (named pipe test is Windows-only, skipped)\n");
#endif
    SUMMARY();
    return g_tests_failed > 0 ? 1 : 0;
}