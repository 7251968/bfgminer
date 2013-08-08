/*
 * Copyright 2013 Luke Dashjr
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uthash.h>

#include "deviceapi.h"
#include "fpgautils.h"
#include "logging.h"
#include "lowlevel.h"
#include "miner.h"
#include "util.h"

BFG_REGISTER_DRIVER(bifury_drv)

const char * const bifury_init_cmds = "flush\ntarget ffffffff\nmaxroll 0\n";

struct bifury_state {
	bytes_t buf;
	uint32_t last_work_id;
	int needwork;
};

static
bool bifury_lowl_match(const struct lowlevel_device_info * const info)
{
	return lowlevel_match_product(info, "bi\xe2\x80\xa2""fury");
}

static
bool bifury_detect_one(const char * const devpath)
{
	char buf[0x40], *p, *q;
	int major, minor, hwrev, chips;
	struct cgpu_info *cgpu;
	const int fd = serial_open(devpath, 0, 10, true);
	applog(LOG_DEBUG, "%s: %s %s",
	       bifury_drv.dname,
	       ((fd == -1) ? "Failed to open" : "Successfully opened"),
	       devpath);
	
	if (unlikely(fd == -1))
		return false;
	
	while (read(fd, buf, sizeof(buf)) == sizeof(buf))
	{}
	
	if (8 != write(fd, "version\n", 8))
	{
		applog(LOG_DEBUG, "%s: Error sending version request", bifury_drv.dname);
		goto err;
	}
	
	if (read(fd, buf, sizeof(buf)) < 1 || strncmp("version ", buf, 8))
	{
		applog(LOG_DEBUG, "%s: Wrong response to version request: %s",
		       bifury_drv.dname, buf);
		goto err;
	}
	
	serial_close(fd);
	
	major = strtol(&buf[8], &p, 10);
	if (p == &buf[8] || p[0] != '.')
		goto parseerr;
	minor = strtol(&p[1], &q, 10);
	if (p == q || strncmp(" rev ", q, 5))
		goto parseerr;
	hwrev = strtol(&q[5], &p, 10);
	if (p == q || strncmp(" chips ", p, 7))
		goto parseerr;
	chips = strtol(&p[7], &q, 10);
	if (p == q || chips < 1)
		goto parseerr;
	
	applog(LOG_DEBUG, "%s: Found firmware %d.%d on hardware rev %d with %d chips",
	       bifury_drv.dname, major, minor, hwrev, chips);
	
	cgpu = malloc(sizeof(*cgpu));
	*cgpu = (struct cgpu_info){
		.drv = &bifury_drv,
		.device_path = strdup(devpath),
		.deven = DEV_ENABLED,
		.procs = chips,
		.threads = 1,
//		.cutofftemp = TODO,
	};
	// NOTE: Xcode's clang has a bug where it cannot find fields inside anonymous unions (more details in fpgautils)
	cgpu->device_fd = -1;
	
	return add_cgpu(cgpu);

parseerr:
	applog(LOG_DEBUG, "%s: Error parsing version response", bifury_drv.dname);
err:
	serial_close(fd);
	return false;
}

static
bool bifury_lowl_probe(const struct lowlevel_device_info * const info)
{
	return vcom_lowl_probe_wrapper(info, bifury_detect_one);
}

static
bool bifury_set_queue_full(const struct cgpu_info * const dev, int needwork)
{
	struct bifury_state * const state = dev->device_data;
	struct thr_info * const master_thr = dev->thr[0];
	const int fd = dev->device_fd;
	if (needwork != -1)
		state->needwork = needwork;
	const bool full = (fd == -1 || !state->needwork);
	if (full == master_thr->queue_full)
		return full;
	for (const struct cgpu_info *proc = dev; proc; proc = proc->next_proc)
	{
		struct thr_info * const thr = proc->thr[0];
		thr->queue_full = full;
	}
	return full;
}

static
bool bifury_thread_init(struct thr_info *master_thr)
{
	struct cgpu_info * const dev = master_thr->cgpu, *proc;
	struct bifury_state * const state = malloc(sizeof(*state));
	if (!state)
		return false;
	*state = (struct bifury_state){
		.buf = BYTES_INIT,
	};
	for (proc = dev; proc; proc = proc->next_proc)
	{
		proc->device_data = state;
		proc->status = LIFE_INIT2;
	}
	bifury_set_queue_full(dev, 0);
	timer_set_now(&master_thr->tv_poll);
	return true;
}

static
void bifury_reinit(struct cgpu_info * const proc)
{
	timer_set_now(&proc->thr[0]->tv_poll);
}

static
void bifury_common_error(struct cgpu_info * const dev, const enum dev_reason reason)
{
	for (struct cgpu_info *proc = dev; proc; proc = proc->next_proc)
	{
		struct thr_info * const thr = proc->thr[0];
		dev_error(proc, reason);
		inc_hw_errors_only(thr);
	}
}

static
bool bifury_queue_append(struct thr_info * const thr, struct work * const work)
{
	const struct cgpu_info * const dev = thr->cgpu->device;
	struct bifury_state * const state = dev->device_data;
	if (bifury_set_queue_full(dev, -1))
		return false;
	
	struct thr_info * const master_thr = dev->thr[0];
	const int fd = dev->device_fd;
	char buf[5 + 0x98 + 1 + 8 + 1];
	memcpy(buf, "work ", 5);
	bin2hex(&buf[5], work->data, 0x4c);
	work->device_id = ++state->last_work_id;
	sprintf(&buf[5 + 0x98], " %08x", work->device_id);
	if (opt_dev_protocol)
		applog(LOG_DEBUG, "%s: DEVPROTO: SEND %s", dev->dev_repr, buf);
	buf[5 + 0x98 + 1 + 8] = '\n';
	if (sizeof(buf) != write(fd, buf, sizeof(buf)))
	{
		applog(LOG_ERR, "%s: Failed to send work", dev->dev_repr);
		return false;
	}
	HASH_ADD(hh, master_thr->work_list, device_id, sizeof(work->device_id), work);
	bifury_set_queue_full(dev, state->needwork - 1);
	return true;
}

static
void bifury_queue_flush(struct thr_info * const thr)
{
	const struct cgpu_info *dev = thr->cgpu;
	if (dev != dev->device)
		return;
	const int fd = dev->device_fd;
	if (fd != -1)
	{
		if (opt_dev_protocol)
			applog(LOG_DEBUG, "%s: DEVPROTO: SEND %s", dev->dev_repr, "flush");
		write(fd, "flush\n", 6);
	}
	bifury_set_queue_full(dev, dev->procs);
}

static
void bifury_handle_cmd(struct cgpu_info * const dev, const char * const cmd)
{
	struct thr_info * const master_thr = dev->thr[0];
	struct bifury_state * const state = dev->device_data;
	struct cgpu_info *proc;
	struct thr_info *thr;
	struct work *work;
	char *p;
	
	if (!strncmp(cmd, "submit ", 7))
	{
		// submit <nonce> <jobid> <timestamp> <chip>
		uint32_t nonce = strtoll(&cmd[7], &p, 0x10);
		const uint32_t jobid = strtoll(&p[1], &p, 0x10);
		strtoll(&p[1], &p, 0x10);  // Ignore timestamp for now
		const int chip = atoi(&p[1]);
		nonce = le32toh(nonce);
		
		proc = dev;
		for (int i = 0; i < chip; ++i)
			proc = proc->next_proc ?: proc;
		thr = proc->thr[0];
		
		HASH_FIND(hh, master_thr->work_list, &jobid, sizeof(jobid), work);
		if (work)
			submit_nonce(thr, work, nonce);
		else
		if (!jobid)
			applog(LOG_DEBUG, "%s: Dummy submit ignored", dev->dev_repr);
		else
			inc_hw_errors2(thr, NULL, &nonce);
	}
	else
	if (!strncmp(cmd, "temp ", 5))
	{
		const int decicelsius = atoi(&cmd[5]);
		if (decicelsius)
		{
			const float celsius = 0.1 * (float)decicelsius;
			for (proc = dev; proc; proc = proc->next_proc)
				proc->temp = celsius;
		}
	}
	else
	if (!strncmp(cmd, "jobs ", 5))
	{
		// jobs <n> ...
		// TODO
	}
	else
	if (!strncmp(cmd, "needwork ", 9))
	{
		const int needwork = atoi(&cmd[9]);
		bifury_set_queue_full(dev, needwork);
		applog(LOG_DEBUG, "%s: needwork=%d", dev->dev_repr, state->needwork);
	}
}

static
void bifury_poll(struct thr_info * const master_thr)
{
	struct cgpu_info * const dev = master_thr->cgpu;
	struct bifury_state * const state = dev->device_data;
	int fd = dev->device_fd;
	char buf[0x100];
	ssize_t r;
	
	if (unlikely(fd == -1))
	{
		fd = serial_open(dev->device_path, 0, 1, true);
		if (unlikely(fd == -1))
		{
			applog(LOG_ERR, "%s: Failed to open %s",
			       dev->dev_repr, dev->device_path);
			bifury_common_error(dev, REASON_THREAD_FAIL_INIT);
			return;
		}
		
		if (opt_dev_protocol)
			applog(LOG_DEBUG, "%s: DEVPROTO: SENDCFG", dev->dev_repr);
		if (sizeof(bifury_init_cmds)-1 != write(fd, bifury_init_cmds, sizeof(bifury_init_cmds)-1))
		{
			applog(LOG_ERR, "%s: Failed to send configuration", dev->dev_repr);
			bifury_common_error(dev, REASON_THREAD_FAIL_INIT);
			return;
		}
		
		dev->device_fd = fd;
		bifury_set_queue_full(dev, dev->procs);
	}
	
	while ( (r = read(fd, buf, sizeof(buf))) > 0)
	{
		bytes_append(&state->buf, buf, r);
		while ( (r = bytes_find(&state->buf, '\n')) >= 0)
		{
			char * const cmd = (void*)bytes_buf(&state->buf);
			cmd[r] = '\0';
			if (opt_dev_protocol)
				applog(LOG_DEBUG, "%s: DEVPROTO: RECV %s", dev->dev_repr, cmd);
			bifury_handle_cmd(dev, cmd);
			bytes_shift(&state->buf, r + 1);
		}
	}
}

struct device_drv bifury_drv = {
	.dname = "bifury",
	.name = "BIF",
	.lowl_match = bifury_lowl_match,
	.lowl_probe = bifury_lowl_probe,
	
	.thread_init = bifury_thread_init,
	.reinit_device = bifury_reinit,
	
	.minerloop = minerloop_queue,
	.queue_append = bifury_queue_append,
	.queue_flush = bifury_queue_flush,
	.poll = bifury_poll,
};