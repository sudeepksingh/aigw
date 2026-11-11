/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Platform handlers: the small programs that do what differs from one
 * gateway to the next. AIGW OS decides (what may be changed, by whom, which
 * values are valid); the handler carries it out on this platform. An OEM
 * replaces a handler with its own (wifi.handler, mgmt.firmware_handler)
 * without touching agd.
 *
 *   wifi-config  save and apply Wi-Fi settings (OpenWRT: uci + wifi reload;
 *                a PC: /etc/aigw/pc-ap.conf + aigw-ap reconf)
 *   firmware     list images, download and check one, activate it
 *                (OpenWRT: sysupgrade)
 *
 * Secrets go in the environment, never on the command line. A handler runs
 * without a shell; what it prints last (stdout or stderr) is its message.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "agd.h"

struct hjob {
	struct uloop_process proc;
	char out[64];                   /* its output file */
	handler_done_t done;
	void *ctx;
};

/* the configured program, else the platform's first that exists; NULL when none */
const char *handler_path(const char *configured, const char *name)
{
	static char buf[4][160];
	static int k;
	static const char *const dirs[] = { "/usr/libexec/aigw", "/usr/local/libexec/aigw", NULL };
	int i;

	if (configured && *configured)
		return !access(configured, X_OK) ? configured : NULL;
	k = (k + 1) % 4;
	for (i = 0; dirs[i]; i++) {
		snprintf(buf[k], sizeof(buf[k]), "%s/%s", dirs[i], name);
		if (!access(buf[k], X_OK))
			return buf[k];
	}
	return NULL;
}

/* the last non-empty line of a file, into msg */
static void last_line(const char *file, char *msg, size_t n)
{
	char line[512];
	FILE *f = fopen(file, "r");

	msg[0] = '\0';
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0])
			snprintf(msg, n, "%s", line);
	}
	fclose(f);
}

static pid_t spawn(const char *prog, char *const argv[], char *const env[], int outfd)
{
	pid_t pid = fork();
	int i;

	if (pid != 0)
		return pid;
	/* the child: its own process group, so a hung handler can be killed whole */
	setpgid(0, 0);
	i = open("/dev/null", O_RDONLY);
	dup2(i, 0);
	dup2(outfd, 1);
	dup2(outfd, 2);
	for (i = 3; i < 256; i++)
		close(i);
	for (i = 0; env && env[i]; i++)
		putenv(env[i]);
	signal(SIGPIPE, SIG_DFL);
	execv(prog, argv);
	fprintf(stderr, "cannot run %s: %s\n", prog, strerror(errno));
	_exit(127);
}

static int out_file(char *path, size_t n)
{
	int fd;

	snprintf(path, n, "/tmp/aigw-handler.XXXXXX");
	fd = mkstemp(path);
	return fd;
}

static void job_exit(struct uloop_process *p, int status)
{
	struct hjob *j = container_of(p, struct hjob, proc);
	char msg[512];
	int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;

	last_line(j->out, msg, sizeof(msg));
	unlink(j->out);
	AG_DEBUG("handler %d: exit %d%s%s", p->pid, code, msg[0] ? ": " : "", msg);
	if (j->done)
		j->done(j->ctx, code, msg);
	free(j);
}

int handler_start(const char *prog, char *const argv[], char *const env[], handler_done_t done, void *ctx)
{
	struct hjob *j = calloc(1, sizeof(*j));
	int fd;

	if (!j)
		return -1;
	if ((fd = out_file(j->out, sizeof(j->out))) < 0) {
		free(j);
		return -1;
	}
	j->done = done;
	j->ctx = ctx;
	j->proc.cb = job_exit;
	j->proc.pid = spawn(prog, argv, env, fd);
	close(fd);
	if (j->proc.pid < 0) {
		unlink(j->out);
		free(j);
		return -1;
	}
	uloop_process_add(&j->proc);
	return 0;
}

int handler_run(const char *prog, char *const argv[], char *out, size_t n, int timeout_ms)
{
	char file[64];
	struct timespec ts = { 0, 20 * 1000 * 1000 };
	int fd, status = 0, waited = 0;
	pid_t pid, r = 0;

	if (out && n)
		out[0] = '\0';
	if ((fd = out_file(file, sizeof(file))) < 0)
		return -1;
	pid = spawn(prog, argv, NULL, fd);
	close(fd);
	if (pid < 0) {
		unlink(file);
		return -1;
	}
	while ((r = waitpid(pid, &status, WNOHANG)) == 0 && waited < timeout_ms) {
		nanosleep(&ts, NULL);
		waited += 20;
	}
	if (r == 0) {
		AG_WARN("handler %s %s: no answer in %d ms", prog, argv[1] ? argv[1] : "", timeout_ms);
		kill(-pid, SIGKILL);
		waitpid(pid, &status, 0);
		unlink(file);
		return -1;
	}
	if (out && n) {
		FILE *f = fopen(file, "r");
		size_t got = f ? fread(out, 1, n - 1, f) : 0;

		out[got] = '\0';
		if (f)
			fclose(f);
	}
	unlink(file);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
