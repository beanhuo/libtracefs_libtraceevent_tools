// SPDX-License-Identifier: LGPL-2.1
/*
 * Original author: Steven Rostedt <rostedt@goodmis.org>
 * https://lore.kernel.org/lkml/20191217183641.1729b821@gandalf.local.home
 *
 * Changed by: Bean Huo <beanhuo@micron.com>
 * Changed by: Tzvetomir Stoyanov (VMware) <tz.stoyanov@gmail.com>
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <traceevent/event-parse.h>
#include <tracefs/tracefs.h>

#define MAJOR(dev)      ((unsigned int) ((dev) >> 20))
#define MINOR(dev)      ((unsigned int) ((dev) & ((1U << 20) - 1)))
struct tep_handle *tep;

#define __weak __attribute__((weak))
#define __noreturn __attribute__((noreturn))

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

static int block_rq_issue_handler(struct trace_seq *s, struct tep_record *record,
				  struct tep_event *event, void *context)
{
	char *comm;
	unsigned long long dev;
	unsigned long long sector;
	unsigned long long nr_sector;
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

struct read_context {
	struct trace_seq s;
	bool *exiting;
	int event_id;
};

static int read_event(struct tep_event *event, struct tep_record *record,
		      int cpu, void *context)
{
	struct read_context *ctx = (struct read_context *)context;

	/* Filter only events with given ID */
	if (event->id == ctx->event_id) {
		trace_seq_init(&ctx->s);
		tep_print_event(event->tep, &ctx->s, record, "%s", TEP_PRINT_INFO);
	}

	if (ctx->exiting)
		return 1;
}

int main(int argc, char *argv[])
{
	const char *trace_systems[] = {"block", NULL};
	struct read_context context;
	struct tep_event *ev;
	int ret;

	tep = tracefs_local_events_system(NULL, trace_systems);
	if (!tep)
		die("Could not allocate tep namespace");

	ret = tracefs_event_enable(NULL, "block", "block_rq_issue");
	if (ret < 0 && !errno) {
		error_no_die("Could not find specifed event");
		goto out;
	}

	ret = tep_register_event_handler(tep, -1, "block", "block_rq_issue",
				   block_rq_issue_handler, NULL);
	if (ret < 0) {
		error_no_die("register event handler failed");
		goto end;
	}

	ev = tep_find_event_by_name(tep, "block", "block_rq_issue");
	if (!ev) {
		error_no_die("failed to get block_rq_issue event\n");
		goto end;
	}
	context.event_id = ev->id;
	context.exiting = &exiting;

	signal(SIGINT, ctl_c_handler);  /* Interrupt from keyboard */

	if (tracefs_trace_on(NULL)) {
		error_no_die("failed to enable trace\n");
		goto end;
	}

	while(1) {
		if (exiting)
			break;
		ret = tracefs_iterate_raw_events(tep, NULL, NULL, 0, read_event, &context);
	}
end:
	tracefs_trace_off(NULL);
	tracefs_instance_file_write(NULL, "trace", "0");
	tracefs_event_disable(NULL, NULL, NULL);
out:
	tep_free(tep);
	return ret;
}
