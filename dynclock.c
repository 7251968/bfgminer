/*
 * Copyright 2012 Luke Dashjr
 * Copyright 2012 nelisky.btc@gmail.com
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "dynclock.h"
#include "miner.h"

void dclk_prepare(struct dclk_data *data)
{
	memset(data, 0, sizeof(*data));
}

bool dclk_updateFreq(struct dclk_data *data, dclk_change_clock_func_t changeclock, struct thr_info *thr)
{
	struct cgpu_info *cgpu = thr->cgpu;
	int i, maxM, bestM;
	double bestR, r;
	bool rv = true;

	for (i = 0; i < data->freqMaxM; i++)
		if (data->maxErrorRate[i + 1] * i < data->maxErrorRate[i] * (i + 20))
			data->maxErrorRate[i + 1] = data->maxErrorRate[i] * (1.0 + 20.0 / i);

	maxM = 0;
	while (maxM < data->freqMDefault && data->maxErrorRate[maxM + 1] < DCLK_MAXMAXERRORRATE)
		maxM++;
	while (maxM < data->freqMaxM && data->errorWeight[maxM] > 150 && data->maxErrorRate[maxM + 1] < DCLK_MAXMAXERRORRATE)
		maxM++;

	bestM = 0;
	bestR = 0;
	for (i = 0; i <= maxM; i++) {
		r = (i + 1 + (i == data->freqM? DCLK_ERRORHYSTERESIS: 0)) * (1 - data->maxErrorRate[i]);
		if (r > bestR) {
			bestM = i;
			bestR = r;
		}
	}

	if (bestM != data->freqM) {
		rv = changeclock(thr, bestM);
	}

	maxM = data->freqMDefault;
	while (maxM < data->freqMaxM && data->errorWeight[maxM + 1] > 100)
		maxM++;
	if ((bestM < (1.0 - DCLK_OVERHEATTHRESHOLD) * maxM) && bestM < maxM - 1) {
		applog(LOG_ERR, "%s %u: frequency drop of %.1f%% detect. This may be caused by overheating. FPGA is shut down to prevent damage.",
		       cgpu->api->name, cgpu->device_id,
		       (1.0 - 1.0 * bestM / maxM) * 100);
		return false;
	}
	return rv;
}

void dclk_gotNonces(struct dclk_data *data)
{
	data->errorCount[data->freqM] *= 0.995;
	data->errorWeight[data->freqM] = data->errorWeight[data->freqM] * 0.995 + 1.0;
}

void dclk_errorCount(struct dclk_data *data, double portion)
{
	data->errorCount[data->freqM] += portion;
}

void dclk_preUpdate(struct dclk_data *data)
{
	data->errorRate[data->freqM] = data->errorCount[data->freqM] / data->errorWeight[data->freqM] * (data->errorWeight[data->freqM] < 100 ? data->errorWeight[data->freqM] * 0.01 : 1.0);
	if (data->errorRate[data->freqM] > data->maxErrorRate[data->freqM])
		data->maxErrorRate[data->freqM] = data->errorRate[data->freqM];
}
