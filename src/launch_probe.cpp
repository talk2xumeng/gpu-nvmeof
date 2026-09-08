/*
 * launch_probe.cpp
 *
 * 一个问题:latency_baseline 里那 21 us 的 "launch + sync",到底
 * 是谁花掉的?
 *
 * 这决定了 GPU-initiated 的独有价值有多大。如果大头是
 * mcDeviceSynchronize 的通知开销,那换成 stream + event 的异步模式
 * 就能省掉一部分 —— 就像调 max_io_size 那次,本以为要靠
 * GPU-initiated 解决的开销,其实改改调用方式就有了。
 *
 * 把一次 kernel 调用拆成四段:
 *
 *   t0 ──[launch 提交]──> t1 ──[调度延迟]──> [kernel 执行] ──> [完成]
 *      ────────────────[sync 返回]────────────────────────────> t2
 *
 *   t1 - t0        launch 的 CPU 侧成本(API 调用本身)
 *   ev_start       kernel 真正开始执行的时刻
 *   ev_end - ev_start   kernel 执行时间(GPU 侧计时)
 *   t2 - t0        CPU 感知到的总时长
 *
 *   (t2-t0) - (ev_end - ev_start) - (t1-t0)  即"其余开销",
 *   包含调度延迟和完成通知。这部分才是 GPU-initiated 可能省掉的。
 *
 * 编译:
 *   mxcc -O2 -x maca -I/opt/maca/include -o launch_probe launch_probe.cpp
 */

#include <mcr/mc_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define ROUNDS		2000
#define WARMUP		200

static inline double
now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

__global__ void
k_empty(void)
{
}

/* 有点工作量的 kernel,用来看执行时间能否被准确分离 */
__global__ void
k_small(unsigned int *p, int iters)
{
	unsigned int idx = blockIdx.x * blockDim.x + threadIdx.x;
	unsigned int v = p[idx];
	int i;

	for (i = 0; i < iters; i++) {
		v = v * 1664525u + 1013904223u;
	}
	p[idx] = v;
}

static int
cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static void
stat_line(const char *name, double *v, int n)
{
	double sum = 0, mean;
	double *s = (double *)malloc(n * sizeof(double));
	int i;

	memcpy(s, v, n * sizeof(double));
	qsort(s, n, sizeof(double), cmp_d);
	for (i = 0; i < n; i++) {
		sum += v[i];
	}
	mean = sum / n;

	printf("  %-22s %8.2f %8.2f %8.2f %8.2f\n",
	       name, mean, s[0], s[n / 2], s[(int)(n * 0.99)]);
	free(s);
}

/* ------------------------------------------------------------------ */

static void
measure(const char *label, int use_work, unsigned int *d_buf, int iters)
{
	double *t_launch, *t_total, *t_exec, *t_other;
	mcEvent_t ev_start, ev_end;
	int r;

	t_launch = (double *)malloc(ROUNDS * sizeof(double));
	t_total  = (double *)malloc(ROUNDS * sizeof(double));
	t_exec   = (double *)malloc(ROUNDS * sizeof(double));
	t_other  = (double *)malloc(ROUNDS * sizeof(double));

	mcEventCreate(&ev_start);
	mcEventCreate(&ev_end);

	for (r = 0; r < WARMUP + ROUNDS; r++) {
		double t0, t1, t2;
		float ms = 0;

		t0 = now_us();
		mcEventRecord(ev_start, 0);
		if (use_work) {
			k_small<<<4, 256>>>(d_buf, iters);
		} else {
			k_empty<<<1, 32>>>();
		}
		mcEventRecord(ev_end, 0);
		t1 = now_us();		/* launch 已提交,尚未执行完 */

		mcDeviceSynchronize();
		t2 = now_us();		/* CPU 确认完成 */

		mcEventElapsedTime(&ms, ev_start, ev_end);

		if (r < WARMUP) {
			continue;
		}

		t_launch[r - WARMUP] = t1 - t0;
		t_total[r - WARMUP]  = t2 - t0;
		t_exec[r - WARMUP]   = ms * 1000.0;	/* ms -> us */
		t_other[r - WARMUP]  = (t2 - t0) - (t1 - t0) - ms * 1000.0;
	}

	printf("\n=== %s ===\n", label);
	printf("  %-22s %8s %8s %8s %8s\n", "段", "mean", "min", "p50", "p99");
	stat_line("launch 提交(CPU)", t_launch, ROUNDS);
	stat_line("kernel 执行(GPU)", t_exec,   ROUNDS);
	stat_line("其余(调度+同步通知)", t_other,  ROUNDS);
	stat_line("总计(CPU 视角)",   t_total,  ROUNDS);

	{
		double l = 0, e = 0, o = 0, t = 0;
		int i;

		for (i = 0; i < ROUNDS; i++) {
			l += t_launch[i];
			e += t_exec[i];
			o += t_other[i];
			t += t_total[i];
		}
		l /= ROUNDS; e /= ROUNDS; o /= ROUNDS; t /= ROUNDS;

		printf("\n  占比: launch %.0f%%, 执行 %.0f%%, 其余 %.0f%%\n",
		       100 * l / t, 100 * e / t, 100 * o / t);
	}

	mcEventDestroy(ev_start);
	mcEventDestroy(ev_end);
	free(t_launch); free(t_total); free(t_exec); free(t_other);
}

/* ------------------------------------------------------------------ */
/* 异步模式对照:不用 mcDeviceSynchronize,改用 event 查询             */
/* ------------------------------------------------------------------ */

static void
measure_async(unsigned int *d_buf, int iters)
{
	double *t_total;
	mcEvent_t ev;
	mcStream_t stream;
	int r;

	t_total = (double *)malloc(ROUNDS * sizeof(double));
	mcStreamCreate(&stream);
	mcEventCreate(&ev);

	for (r = 0; r < WARMUP + ROUNDS; r++) {
		double t0, t2;

		t0 = now_us();
		k_small<<<4, 256, 0, stream>>>(d_buf, iters);
		mcEventRecord(ev, stream);

		/* 自旋查询而不是阻塞等待。真实场景里这段时间可以做别的事,
		 * 这里只是想看"最快多久能确认完成"。 */
		while (mcEventQuery(ev) != mcSuccess) {
			;
		}
		t2 = now_us();

		if (r < WARMUP) {
			continue;
		}
		t_total[r - WARMUP] = t2 - t0;
	}

	printf("\n=== 异步模式(stream + eventQuery 自旋)===\n");
	printf("  %-22s %8s %8s %8s %8s\n", "段", "mean", "min", "p50", "p99");
	stat_line("总计", t_total, ROUNDS);

	mcEventDestroy(ev);
	mcStreamDestroy(stream);
	free(t_total);
}

/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
	int dev = (argc > 1) ? atoi(argv[1]) : 0;
	int iters = (argc > 2) ? atoi(argv[2]) : 1000;
	unsigned int *d_buf = NULL;

	if (mcSetDevice(dev) != mcSuccess) {
		fprintf(stderr, "mcSetDevice(%d) 失败\n", dev);
		return 1;
	}
	if (mcMalloc((void **)&d_buf, 1024 * sizeof(unsigned int)) != mcSuccess) {
		fprintf(stderr, "mcMalloc 失败\n");
		return 1;
	}
	mcMemset(d_buf, 0, 1024 * sizeof(unsigned int));

	printf("GPU %d, kernel 内循环 %d 次, %d 轮(丢弃前 %d 轮)\n",
	       dev, iters, ROUNDS, WARMUP);

	measure("空 kernel", 0, d_buf, 0);
	measure("有工作量的 kernel", 1, d_buf, iters);
	measure_async(d_buf, iters);

	printf("\n---------------------------------------------------------\n");
	printf("怎么读这组数:\n");
	printf("  - 若\"其余\"占大头,说明开销在调度和完成通知上。\n");
	printf("    异步模式那一行若明显更快,说明换调用方式就能省,\n");
	printf("    不必等 GPU-initiated。\n");
	printf("  - 若\"launch 提交\"占大头,是 API 本身的 CPU 成本,\n");
	printf("    只有 kernel 内直发能绕开。\n");
	printf("  - \"kernel 执行\"是真实工作量,谁也省不掉。\n");

	mcFree(d_buf);
	return 0;
}
