/*
 * blk_pattern.h  --  逐块可辨识的数据 pattern,host 和 kernel 共用
 *
 * 为什么不再用单字节填充(0x5A / 0xBB):
 * kernel 每轮读写显存同一块、盘上不同 LBA 时,单字节 pattern 下
 * 漏块、串块、LBA 和数据对不上全都长得一样,查不出来。
 *
 * 块布局(按 8 字节小端字):
 *   word[0] = BLK_MAGIC ^ seed      低字节是 seed,可以认出"别的轮次"
 *   word[1] = lba                   块自报的 LBA,可以认出"串块"
 *   word[k] = mix(seed, lba, k)     k >= 2,块内每个字都不同
 *
 * 所以任何一块读回来都能判定成下面之一:
 *   OK       完全一致
 *   FILL     整块单字节(0xBB 没落地 / 0xCC 没写 / 0xEE 显存旧值 ...)
 *   CROSS    是本轮 pattern,但属于另一个 LBA  -> 串块
 *   OLDSEED  是 pattern,但 seed 不同            -> 上一轮残留
 *   BAD      其余(部分写入、乱码)
 *
 * seed 每次运行都应该换(Makefile 里按时间取),否则上一轮留在盘上的
 * 数据会冒充本轮结果。
 *
 * io_bytes 必须是 8 的整数倍(扇区大小都满足)。
 */

#ifndef BLK_PATTERN_H
#define BLK_PATTERN_H

#include <stdint.h>

#if defined(__MACACC__) || defined(__CUDACC__) || defined(USE_MACA_DEVICE)
  #define BLK_FN	__host__ __device__ static inline
  #define BLK_DEVICE	1
#else
  #define BLK_FN	static inline
  #define BLK_DEVICE	0
#endif

#define BLK_MAGIC	0x4b4c424d564e4700ULL	/* 低字节留给 seed */

/* 几种单字节底色,host 侧统一在这里定义,分类时要用 */
#define BLK_RPAT	0xbb	/* 读:显存预填,被 target 覆盖才算落地 */
#define BLK_WPAT	0x5a	/* 旧式写 pattern(-K) */
#define BLK_CPAT	0xcc	/* 盘上涂色:还是它 = 没写到 */
#define BLK_EPAT	0xee	/* 显存底色(tst/pit7):读到它 = 网卡取到了 kernel 写之前的旧值 */

BLK_FN uint64_t
blk_splitmix64(uint64_t z)
{
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

BLK_FN uint64_t
blk_word(uint8_t seed, uint64_t lba, uint32_t k)
{
	if (k == 0) {
		return BLK_MAGIC ^ seed;
	}
	if (k == 1) {
		return lba;
	}
	return blk_splitmix64(blk_splitmix64(lba ^ ((uint64_t)seed << 56)) + k);
}

#if !BLK_DEVICE

#include <stddef.h>
#include <string.h>

static inline uint64_t
blk_get64(const uint8_t *p, size_t off)
{
	uint64_t v;

	memcpy(&v, p + off, 8);
	return v;
}

static inline void
blk_fill(uint8_t *p, size_t len, uint8_t seed, uint64_t lba)
{
	uint32_t k;

	for (k = 0; k < len / 8; k++) {
		uint64_t v = blk_word(seed, lba, k);

		memcpy(p + (size_t)k * 8, &v, 8);
	}
}

enum blk_verdict {
	BLK_OK = 0,
	BLK_FILL,
	BLK_CROSS,
	BLK_OLDSEED,
	BLK_BAD,
};

struct blk_result {
	enum blk_verdict	v;
	uint8_t			fill;		/* FILL:底色 */
	uint8_t			seed;		/* OLDSEED:块里的 seed */
	uint64_t		lba;		/* CROSS/OLDSEED:块自报的 LBA */
	size_t			bad_off;	/* 第一个不一致的字节偏移 */
};

static inline enum blk_verdict
blk_check(const uint8_t *p, size_t len, uint8_t seed, uint64_t lba,
	  struct blk_result *r)
{
	uint64_t w0 = blk_get64(p, 0);
	uint32_t k;
	size_t q;

	memset(r, 0, sizeof(*r));
	r->bad_off = len;
	for (k = 0; k < len / 8; k++) {
		if (blk_get64(p, (size_t)k * 8) != blk_word(seed, lba, k)) {
			size_t b;
			uint64_t e = blk_word(seed, lba, k);
			uint64_t g = blk_get64(p, (size_t)k * 8);

			for (b = 0; b < 8 && ((e >> (b * 8)) & 0xff) ==
					     ((g >> (b * 8)) & 0xff); b++) {
			}
			r->bad_off = (size_t)k * 8 + b;
			break;
		}
	}
	if (r->bad_off == len) {
		return r->v = BLK_OK;
	}

	for (q = 1; q < len && p[q] == p[0]; q++) {
	}
	if (q == len) {
		r->fill = p[0];
		return r->v = BLK_FILL;
	}

	if ((w0 & ~0xffULL) == BLK_MAGIC) {
		r->seed = (uint8_t)(w0 & 0xff);
		r->lba = blk_get64(p, 8);
		if (r->seed != seed) {
			return r->v = BLK_OLDSEED;
		}
		if (r->lba != lba) {
			return r->v = BLK_CROSS;
		}
	}
	return r->v = BLK_BAD;
}

static inline char
blk_map_char(const struct blk_result *r)
{
	switch (r->v) {
	case BLK_OK:		return '.';
	case BLK_CROSS:		return 'X';
	case BLK_OLDSEED:	return 'O';
	case BLK_BAD:		return '?';
	case BLK_FILL:
		switch (r->fill) {
		case BLK_RPAT:	return 'R';
		case BLK_CPAT:	return 'C';
		case BLK_EPAT:	return 'S';
		case BLK_WPAT:	return 'W';
		case 0x00:	return 'Z';
		default:	return 'F';
		}
	}
	return '?';
}

#endif /* !BLK_DEVICE */

#endif /* BLK_PATTERN_H */
