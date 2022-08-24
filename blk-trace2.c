// SPDX-License-Identifier: LGPL-2.1
/*
 * Original author: Steven Rostedt <rostedt@goodmis.org>
 * https://lore.kernel.org/lkml/20191217183641.1729b821@gandalf.local.home
 *
 * Changed by: Bean Huo <beanhuo@micron.com>
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <traceevent/event-parse.h>
#include <traceevent/kbuffer.h>
#include <traceevent/trace-seq.h>
#include <tracefs.h>
#include <assert.h>

#define MAJOR(dev)      ((unsigned int) ((dev) >> 20))
#define MINOR(dev)      ((unsigned int) ((dev) & ((1U << 20) - 1)))
struct tep_handle *tep;
struct kbuffer *kbuf;
static int page_size;

#define __weak __attribute__((weak))
#define __noreturn __attribute__((noreturn))

static int block_rq_issue_handler(struct trace_seq *s, struct tep_record *record,
				  struct tep_event *event, void *context);
static bool exiting = false;
static void ctl_c_handler(int sig)
{
	printf("Caught signal %d\n", sig);
	exiting = true;
}

void print_error_msg(const char *fmt, va_list ap)
{
	fprintf(stderr, "Error:  ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
}

void __weak __noreturn die(const char *fmt, ...)
{
	va_list ap;
	int ret = errno ? errno : -1;

	va_start(ap, fmt);
	print_error_msg(fmt, ap);
	va_end(ap);

	exit(ret);
}

void __weak  error_no_die(const char *fmt, ...)
{
	va_list ap;

	if (errno)
		perror("Error: ");

	va_start(ap, fmt);
	print_error_msg(fmt, ap);
	va_end(ap);
}

static char *read_file(const char *file, size_t *file_size)
{
	char *line;
	char *buf = NULL;
	FILE *fp;
	size_t len = 0;
	size_t size = 0;
	int s;
	int ret;

	fp = fopen(file, "r");
	if (!fp) {
		error_no_die("Could not open file %s", file);
		return NULL;
	}

	while ((ret = getline(&line, &len, fp)) > 0) {
		s = strlen(line);
		buf = realloc(buf, size + s + 1);
		if (!buf) {
			error_no_die("Allocating memory to read %s\n", file);
			fclose(fp);
			free(line);
			return NULL;
		}
		strcpy(buf + size, line);
		size += s;
	}

	free(line);
	fclose(fp);
	*file_size = size;
	return buf;
}

static void read_raw_buffer(int cpu, const char *buffer)
{
	char buf[page_size];
	int fd;
	int r;
	unsigned long long ts;

	struct tep_record record;
	struct tep_event *event;

	fd = open(buffer, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		error_no_die("Failed to open %s", buffer);
		return;

	}

	event = tep_find_event_by_name(tep, "block", "block_rq_issue");
	while ((r = read(fd, buf, page_size)) > 0) {
		kbuffer_load_subbuffer(kbuf, buf);

		for (;;) {
			record.data = kbuffer_read_event(kbuf, &record.ts);
			if (!record.data)
				break;
			record.cpu = cpu;
			record.size = kbuffer_event_size(kbuf);
			record.missed_events = kbuffer_missed_events(kbuf);

			block_rq_issue_handler(NULL, &record, event, NULL);

			kbuffer_next_event(kbuf, &ts);
		}
	}

	close(fd);
}

static int block_rq_issue_handler(struct trace_seq *s, struct tep_record *record,
				  struct tep_event *event, void *context)
{
	char *comm;
	unsigned long long dev;
	unsigned int sector;
	unsigned int nr_sector;
	char *rwbs;
	unsigned long long pid  = 0;
	int len;
	unsigned long long ts;
	int cpu;

	if (!tep_get_common_field_val(NULL, event, "common_pid", record, &pid, 1)) {
		comm = tep_get_field_raw(NULL, event, "comm", record, &len, 0);
		if (comm == NULL)
			printf("Unknow COMM\n");
	} else {
		printf("Unknow PID\n");
	}

	ts = record->ts;
	cpu = record->cpu;
	if (tep_get_field_val(NULL, event, "dev", record, &dev, 0))
		printf("Unknow DEV_ID\n");

	if (tep_get_field_val(NULL, event, "sector", record, &sector, 0))
		printf("Unknow DEV_ID\n");

	if (tep_get_field_val(NULL, event, "nr_sector", record, &nr_sector, 0))
		printf("Unknow LEN\n");

	rwbs = tep_get_field_raw(NULL, event, "rwbs", record, &len, 0);
	if (rwbs == NULL)
		printf("Unknow OP\n");

	printf("CPU %d, TS %llu.%llu, pid %lld, comm %s, %d:%d, LBA %x, sectors %d, %s\n", cpu,
	       ts/1000000000, ts%1000000000,
	       pid, comm, MAJOR(dev), MINOR(dev), sector, nr_sector, rwbs);

	return 0;
}

static int tracefs_load_format(struct tep_handle *tep, const char *tracefs, const char *system,
			 const char *event)
{
	int ret;
	size_t size;
	char *format;
	char *buf;

	ret = asprintf(&format, "%s/events/%s/%s/format", tracefs, system, event);
	if (ret < 0) {
		error_no_die("Could not allocate memory for format path\n");
		return ret;
	}
	buf = read_file(format, &size);
	assert(buf);
	tep_parse_event(tep, buf, size, system);
	free(format);
	free(buf);

	return 0;
}

int main(int argc, char *argv[])
{
	char *tracefs = NULL;
	enum kbuffer_long_size lsize;
	enum kbuffer_endian endian;
	struct stat st;
	size_t size;
	char *header_page;
	char *per_cpu;
	char *buf;
	int ret;
	int i;
	int cpus;

	page_size = getpagesize();
	tracefs = tracefs_tracing_dir();
	if (!tracefs)
		die("Can not find tracefs");

	tep = tep_alloc();
	if (!tep)
		die("Could not allocate tep namespace");

	lsize = sizeof(long) == 4 ? KBUFFER_LSIZE_4 : KBUFFER_LSIZE_8;
	if (tep_is_bigendian())
		endian = KBUFFER_ENDIAN_BIG;
	else
		endian = KBUFFER_ENDIAN_LITTLE;

	kbuf = kbuffer_alloc(lsize, endian);
	if (!kbuf) {
		error_no_die("Could not allocate kbuffer handle");
		tep_free(tep);
		return errno;
	}

	ret = asprintf(&header_page, "%s/events/header_page", tracefs);
	if (ret < 0) {
		error_no_die("Could not allocate memory for header page");
		ret = errno;
		goto out;

	}

	buf = read_file(header_page, &size);
	assert(buf);
	tep_parse_header_page(tep, buf, size, sizeof(long));
	free(header_page);

	ret = tracefs_load_format(tep, tracefs, "block", "block_rq_issue");
	if (ret < 0) {
		error_no_die("Could not load format\n");
		goto out;
	}

	ret = tracefs_event_enable(NULL, "block", "block_rq_issue");
	if (ret < 0 && !errno) {
		error_no_die("Could not find specifed event");
		goto out;
	}

	cpus = sysconf(_SC_NPROCESSORS_ONLN);

	ret = asprintf(&per_cpu, "%s/per_cpu", tracefs);
	if (ret == -1 || !per_cpu) {
		error_no_die("Could not allocate memory for per_cpu path");
		goto end;
	}

	signal(SIGINT, ctl_c_handler);  /* Interrupt from keyboard */

	if (tracefs_trace_on(NULL)) {
		error_no_die("failed to enable trace\n");
		goto end;
	}

	while(1) {
		if (exiting)
			break;
		for (i = 0; i < cpus; i++) {
			char *raw_buf;
			char *cpu;

			if (exiting)
				break;

			ret = asprintf(&cpu, "%s/cpu%d", per_cpu, i);
			if (ret < 0) {
				error_no_die("Could not allocate memory for cpu buffer %d name", i);
				goto end;
			}

			ret = stat(cpu, &st);
			if (ret < 0 || !S_ISDIR(st.st_mode)) {
				free(cpu);
				continue;
			}

			ret = asprintf(&raw_buf, "%s/trace_pipe_raw", cpu);
			if (ret < 0) {
				error_no_die("Could not allocate memory for cpu %d raw buffer name", i);
				goto end;
			}

			read_raw_buffer(i, raw_buf);
			free(raw_buf);
			free(cpu);
		}
	}
end:
	tracefs_trace_off(NULL);
	tracefs_instance_file_write(NULL, "trace", "0");
	tracefs_event_disable(NULL, NULL, NULL);
	free(per_cpu);
out:
	tep_free(tep);
	return ret;
}
