#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>

volatile sig_atomic_t stopFlag = 0;

#define TICK_INTERVAL_MS 500

#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))

int m_vi_get_frame();
int m_vi_init();
int m_vi_deinit();

static uint64_t get_monotonic_time_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}


void handle_sigint(int signum) {
	printf("Sign %d\n", signum);
    stopFlag = 1;
}

int count = 0;

int main(void)
{

	signal(SIGINT, handle_sigint);

	printf("Initializing VI system...\n");
	if (m_vi_init() != 0) {
		printf("VI initialization failed.\n");
		return -1;
	}

	uint64_t last_tick = get_monotonic_time_ms();
	uint64_t current_tick;

#if 1
	while (stopFlag == 0) {
		current_tick = get_monotonic_time_ms();
		uint64_t elapsed = current_tick - last_tick;
		last_tick = current_tick;
		count++;

		// 精确延迟 33ms
		struct timespec req = {
			.tv_sec = 0,
			.tv_nsec = TICK_INTERVAL_MS * 1000000L
		};

		nanosleep(&req, NULL);

	}
#endif


	m_vi_deinit();

	return 0;
}

