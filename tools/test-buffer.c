#include "../squeezelite.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

log_level loglevel = lERROR;
const char *logtime(void) { return "test"; }
void logprint(const char *format, ...) { (void)format; }

static void fill_wrapped(struct buffer *buffer, size_t read, size_t write) {
	unsigned value = 1;
	buffer->readp = buffer->buf + read;
	buffer->writep = buffer->buf + write;
	for (u8_t *p = buffer->readp; p != buffer->writep; p = p + 1 == buffer->wrap ? buffer->buf : p + 1)
		*p = (u8_t)value++;
}

static void verify_prefix(struct buffer *buffer, unsigned count) {
	for (unsigned i = 0; i < count; ++i) assert(buffer->readp[i] == i + 1);
}

int main(void) {
	struct buffer buffer = {0};
	buf_init(&buffer, 16);
	assert(buffer.buf && buffer.size == 16);
	_buf_inc_writep(&buffer, 15);
	assert(_buf_used(&buffer) == 15);
	_buf_inc_readp(&buffer, 14);
	assert(_buf_used(&buffer) == 1);
	_buf_inc_writep(&buffer, 18);
	assert(buffer.writep == buffer.buf + 1);

	buf_adjust(&buffer, 0);
	assert(buffer.size == 16);
	mutex_lock(buffer.mutex);
	_buf_resize(&buffer, 32);
	mutex_unlock(buffer.mutex);
	assert(buffer.buf && buffer.size == 32 && buffer.base_size == 32);

	fill_wrapped(&buffer, 24, 8);
	_buf_unwrap(&buffer, 16);
	assert(_buf_cont_read(&buffer) >= 16);
	verify_prefix(&buffer, 16);

	fill_wrapped(&buffer, 24, 22);
	_buf_unwrap(&buffer, 16);
	assert(_buf_cont_read(&buffer) >= 16);
	verify_prefix(&buffer, 16);

	buf_destroy(&buffer);
	puts("buffer wrap and resize tests passed");
	return 0;
}
