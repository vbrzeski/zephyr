/*
 * Copyright (c) 2026 Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/sys/ring_buffer.h>

#define RB_SIZE    7U
#define GUARD_SIZE 8U
#define GUARD_BYTE 0xa5U

static uint8_t storage[GUARD_SIZE + RB_SIZE + GUARD_SIZE];
static uint8_t *const rb_data = &storage[GUARD_SIZE];
static struct ring_buf rb;

static void check_guards(void)
{
	for (size_t i = 0; i < GUARD_SIZE; i++) {
		zassert_equal(storage[i], GUARD_BYTE, "write before data area");
		zassert_equal(storage[GUARD_SIZE + RB_SIZE + i], GUARD_BYTE,
			      "write after data area");
	}
}

/* Fill and drain the whole ring buffer to check it stays within its data area */
static void check_usable(void)
{
	uint8_t tmp[RB_SIZE];

	zassert_equal(ring_buf_size_get(&rb) + ring_buf_space_get(&rb), RB_SIZE);
	(void)ring_buf_get(&rb, tmp, sizeof(tmp));
	zassert_true(ring_buf_is_empty(&rb));
	memset(tmp, 0x5a, sizeof(tmp));
	zassert_equal(ring_buf_put(&rb, tmp, sizeof(tmp)), RB_SIZE);
	zassert_equal(ring_buf_get(&rb, tmp, sizeof(tmp)), RB_SIZE);
	zassert_true(ring_buf_is_empty(&rb));
	check_guards();
}

static void check_content(const char *expected)
{
	uint8_t tmp[RB_SIZE] = {0};
	size_t len = strlen(expected);

	zassert_equal(ring_buf_get(&rb, tmp, sizeof(tmp)), len);
	zassert_mem_equal(tmp, expected, len);
}

static void recover_before(void *fixture)
{
	ARG_UNUSED(fixture);

	memset(storage, GUARD_BYTE, sizeof(storage));
	ring_buf_init(&rb, RB_SIZE, rb_data);
}

ZTEST(ringbuffer_recover, test_recover_empty)
{
	zassert_ok(ring_buf_recover(&rb, RB_SIZE, rb_data));
	zassert_true(ring_buf_is_empty(&rb));
	zassert_equal(ring_buf_capacity_get(&rb), RB_SIZE);
	check_usable();
}

ZTEST(ringbuffer_recover, test_recover_keeps_data)
{
	zassert_equal(ring_buf_put(&rb, "abc", 3), 3);
	rb.buffer = NULL;

	zassert_ok(ring_buf_recover(&rb, RB_SIZE, rb_data));
	zassert_equal_ptr(rb.buffer, rb_data);
	check_content("abc");
	check_usable();
}

ZTEST(ringbuffer_recover, test_recover_keeps_wrapped_data)
{
	uint8_t tmp[RB_SIZE] = {0};

	/* Move both indices into the second lap, near the end of the data area */
	zassert_equal(ring_buf_put(&rb, tmp, RB_SIZE), RB_SIZE);
	zassert_equal(ring_buf_get(&rb, tmp, RB_SIZE), RB_SIZE);
	zassert_equal(ring_buf_put(&rb, tmp, RB_SIZE - 2), RB_SIZE - 2);
	zassert_equal(ring_buf_get(&rb, tmp, RB_SIZE - 2), RB_SIZE - 2);
	zassert_equal(ring_buf_put(&rb, "wrap", 4), 4);

	zassert_ok(ring_buf_recover(&rb, RB_SIZE, rb_data));
	check_content("wrap");
	check_usable();
}

ZTEST(ringbuffer_recover, test_recover_keeps_full)
{
	zassert_equal(ring_buf_put(&rb, "abcdefg", RB_SIZE), RB_SIZE);

	zassert_ok(ring_buf_recover(&rb, RB_SIZE, rb_data));
	zassert_true(ring_buf_is_full(&rb));
	check_content("abcdefg");
	check_usable();
}

ZTEST(ringbuffer_recover, test_recover_size)
{
	rb.size = 0;
	zassert_equal(ring_buf_recover(&rb, RB_SIZE, rb_data), -EINVAL);

	ring_buf_init(&rb, RB_SIZE, rb_data);
	zassert_equal(ring_buf_recover(&rb, RB_SIZE - 1, rb_data), -EINVAL);

	ring_buf_init(&rb, RB_SIZE, rb_data);
	zassert_ok(ring_buf_recover(&rb, RB_SIZE + 1, rb_data));

	rb.size = RING_BUFFER_MAX_SIZE + 1;
	rb.read_idx = 0;
	rb.write_idx = 0;
	zassert_equal(ring_buf_recover(&rb, UINT32_MAX, rb_data), -EINVAL);
}

/*
 * Every read/write index pair around the valid [0, 2N) range: only consistent
 * pairs are accepted, and an accepted ring buffer holds exactly the bytes its
 * indices describe.
 */
ZTEST(ringbuffer_recover, test_recover_all_indices)
{
	const uint32_t lim = 2U * RB_SIZE;

	for (uint32_t r = 0; r < lim + 3U; r++) {
		for (uint32_t w = 0; w < lim + 3U; w++) {
			uint32_t used = (w + lim - r) % lim;
			bool valid = r < lim && w < lim && used <= RB_SIZE;
			uint8_t tmp[RB_SIZE];
			int ret;

			recover_before(NULL);
			for (uint32_t i = 0; i < RB_SIZE; i++) {
				rb_data[i] = i + 1U;
			}
			rb.read_idx = r;
			rb.write_idx = w;
			rb.buffer = NULL;

			ret = ring_buf_recover(&rb, RB_SIZE, rb_data);
			zassert_equal(ret, valid ? 0 : -EINVAL, "read %u write %u", r, w);
			zassert_equal_ptr(rb.buffer, rb_data);
			if (!valid) {
				continue;
			}

			zassert_equal(ring_buf_size_get(&rb), used, "read %u write %u", r, w);
			zassert_equal(ring_buf_get(&rb, tmp, sizeof(tmp)), used);
			for (uint32_t i = 0; i < used; i++) {
				zassert_equal(tmp[i], (r + i) % RB_SIZE + 1U,
					      "read %u write %u byte %u", r, w, i);
			}
			check_usable();
		}
	}
}

#ifdef CONFIG_RING_BUFFER
ZTEST(ringbuffer_recover, test_recover_releases_claims)
{
	uint8_t *ptr;

	zassert_equal(ring_buf_put(&rb, "ab", 2), 2);
	zassert_equal(ring_buf_put_claim(&rb, &ptr, 3), 3);
	memcpy(ptr, "xyz", 3);
	zassert_equal(ring_buf_get_claim(&rb, &ptr, 1), 1);

	zassert_ok(ring_buf_recover(&rb, RB_SIZE, rb_data));
	zassert_equal(ring_buf_put_claim(&rb, &ptr, RB_SIZE), RB_SIZE - 2);
	zassert_ok(ring_buf_put_finish(&rb, 0));
	check_content("ab");
	check_usable();
}
#endif /* CONFIG_RING_BUFFER */

ZTEST_SUITE(ringbuffer_recover, NULL, NULL, recover_before, NULL, NULL);
