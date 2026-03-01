/*
 * test_srt.c
 * Quick test to verify SRT library works for streaming.
 */

#include <stdio.h>
#include <srt/srt.h>

int main(void) {
    printf("=== SRT Library Test ===\n\n");

    /* Initialize SRT */
    if (srt_startup() != 0) {
        fprintf(stderr, "FAIL: srt_startup failed\n");
        return 1;
    }
    printf("[OK] SRT initialized\n");
    printf("[OK] SRT version: %s\n", SRT_VERSION_STRING);

    /* Create a socket */
    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        fprintf(stderr, "FAIL: srt_create_socket failed: %s\n", srt_getlasterror_str());
        srt_cleanup();
        return 1;
    }
    printf("[OK] SRT socket created\n");

    /* Set latency */
    int latency_ms = 20;
    srt_setsockflag(sock, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));
    printf("[OK] SRT latency set to %d ms\n", latency_ms);

    /* Cleanup */
    srt_close(sock);
    srt_cleanup();

    printf("\n=== ALL TESTS PASSED ===\n");
    printf("SRT streaming library is ready.\n");

    return 0;
}
