/*
    JACK transport client interface -- runs in the client process.

    Copyright (C) 2001-2003 Paul Davis
    Copyright (C) 2003 Jack O'Quin
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public License
    as published by the Free Software Foundation; either version 2.1
    of the License, or (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this program; if not, write to the Free
    Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
    02111-1307, USA.
*/

#include <config.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <jack/atomicity.h>
#include <jack/internal.h>
#include "local.h"


/********************* Internal functions *********************/

/* generate a unique non-zero ID, different for each call */
jack_unique_t
jack_generate_unique_id (jack_control_t *ectl)
{
	/* The jack_unique_t is an opaque type. */
	return exchange_and_add(&ectl->seq_number, 1);
}

static inline void
jack_read_frame_time (const jack_client_t *client, jack_frame_timer_t *copy)
{
	int tries = 0;

	do {
		/* throttle the busy wait if we don't get 
		   the answer very quickly.
		*/

		if (tries > 10) {
			usleep (20);
			tries = 0;
		}

		*copy = client->engine->frame_timer;

		tries++;

	} while (copy->guard1 != copy->guard2);
}

/* copy a JACK transport position structure (thread-safe) */
void
jack_transport_copy_position (jack_position_t *from, jack_position_t *to)
{
	int tries = 0;
	long timeout = 1000;

	do {
		/* throttle the busy wait if we don't get the answer
		 * very quickly. */
		if (tries > 10) {
			usleep (20);
			tries = 0;

			/* debug code to avoid system hangs... */
			if (--timeout == 0) {
				jack_error("hung in loop copying position");
				abort();
			}
		}
		*to = *from;
		tries++;

	} while (to->unique_1 != to->unique_2);
}

static inline int
jack_transport_request_new_pos (jack_client_t *client, jack_position_t *pos)
{
	jack_control_t *ectl = client->engine;

	/* distinguish this request from all others */
	pos->unique_1 = pos->unique_2 = jack_generate_unique_id(ectl);

	/* clients may not set these fields */
	pos->usecs = ectl->current_time.usecs;
	pos->frame_rate = ectl->current_time.frame_rate;

	/* carefully copy requested postion into shared memory */
	jack_transport_copy_position (pos, &ectl->request_time);
	
	return 0;
}


/******************** Callback invocations ********************/

void
jack_call_sync_client (jack_client_t *client)
{
	jack_client_control_t *control = client->control;
	jack_control_t *ectl = client->engine;

	/* Make sure still active and slow-sync; active_slowsync is
	 * set in a critical section; sync_cb is not. */
	if ((ectl->new_pos || control->sync_poll || control->sync_new) &&
	    control->active_slowsync) {

		if (control->sync_cb (ectl->transport_state,
				      &ectl->current_time,
				      control->sync_arg)) {

			if (control->sync_poll) {
				control->sync_poll = 0;
				ectl->sync_remain--;
			}
		}
		control->sync_new = 0;
	}
}

void
jack_call_timebase_master (jack_client_t *client)
{
	jack_client_control_t *control = client->control;
	jack_control_t *ectl = client->engine;
	int new_pos = (int) ectl->pending_pos;

	
	/* Make sure this is still the master; is_timebase is set in a
	 * critical section; timebase_cb is not. */
	if (control->is_timebase) { 

		if (control->timebase_new) {	/* first callback? */
			control->timebase_new = 0;
			new_pos = 1;
		}


		if ((ectl->transport_state == JackTransportRolling) ||
		    new_pos) {

			control->timebase_cb (ectl->transport_state,
					      control->nframes,
					      &ectl->pending_time,
					      new_pos,
					      control->timebase_arg);
		}

	} else {

		/* another master took over, so resign */
		control->timebase_cb = NULL;
		control->timebase_arg = NULL;
	}
}


/************************* API functions *************************/

jack_nframes_t
jack_get_current_transport_frame (const jack_client_t *client)
{
 	jack_position_t position;
	float usecs;
	jack_nframes_t elapsed;
	jack_transport_state_t tstate;

	/* get the current transport position information. 
	   this is thread-safe and atomic with respect
	   to the structure contents.
	*/

	tstate = jack_transport_query (client, &position);
	
	if (tstate != JackTransportRolling) {
		return position.frame;
	}
	
	/* compute the elapsed usecs then audio frames since
	   the transport info was last updated
	*/
	
	usecs = jack_get_microseconds() - position.usecs;
	elapsed = (jack_nframes_t) floor ((((float) position.frame_rate)
					   / 1000000.0f) * usecs);

	/* return the estimated transport frame position
	 */
	
	return position.frame + elapsed;
}	

jack_nframes_t
jack_frames_since_cycle_start (const jack_client_t *client)
{
	float usecs;
	jack_control_t *ectl = client->engine;

	usecs = jack_get_microseconds() - ectl->current_time.usecs;
	return (jack_nframes_t) floor ((((float) ectl->current_time.frame_rate)
					/ 1000000.0f) * usecs);
}

jack_nframes_t
jack_frame_time (const jack_client_t *client)
{
	jack_frame_timer_t current;
	float usecs;
	jack_nframes_t elapsed;
	jack_control_t *ectl = client->engine;

	jack_read_frame_time (client, &current);
	
	usecs = jack_get_microseconds() - current.stamp;
	elapsed = (jack_nframes_t)
		floor ((((float) ectl->current_time.frame_rate)
			/ 1000000.0f) * usecs);
	
	return current.frames + elapsed;
}

jack_nframes_t
jack_get_sample_rate (jack_client_t *client)
{
	return client->engine->current_time.frame_rate;
}

int
jack_set_sample_rate_callback (jack_client_t *client,
			       JackSampleRateCallback callback, void *arg)
{
	if (client->control->active) {
		jack_error ("You cannot set callbacks on an active client.");
		return -1;
	}
	client->control->srate_arg = arg;
	client->control->srate = callback;

	/* Now invoke it */

	callback (client->engine->current_time.frame_rate, arg);

	return 0;
}

int  
jack_release_timebase (jack_client_t *client)
{
	int rc;
	jack_request_t req;
	jack_client_control_t *ctl = client->control;

	req.type = ResetTimeBaseClient;
	req.x.client_id = ctl->id;

	rc = jack_client_deliver_request (client, &req);
	if (rc == 0) {
		ctl->timebase_cb = NULL;
		ctl->timebase_arg = NULL;
	}
	return rc;
}

int  
jack_set_sync_callback (jack_client_t *client,
			JackSyncCallback sync_callback, void *arg)
{
	jack_client_control_t *ctl = client->control;
	jack_request_t req;
	int rc;

	if (sync_callback)
		req.type = SetSyncClient;
	else
		req.type = ResetSyncClient;
	req.x.client_id = ctl->id;

	rc = jack_client_deliver_request (client, &req);
	if (rc == 0) {
		ctl->sync_cb = sync_callback;
		ctl->sync_arg = arg;
	}
	return rc;
}

int  
jack_set_sync_timeout (jack_client_t *client, jack_time_t usecs)
{
	jack_request_t req;

	req.type = SetSyncTimeout;
	req.x.timeout = usecs;

	return jack_client_deliver_request (client, &req);
}

int  
jack_set_timebase_callback (jack_client_t *client, int conditional,
			    JackTimebaseCallback timebase_cb, void *arg)
{
	int rc;
	jack_request_t req;
	jack_client_control_t *ctl = client->control;

	req.type = SetTimeBaseClient;
	req.x.timebase.client_id = ctl->id;
	req.x.timebase.conditional = conditional;

	rc = jack_client_deliver_request (client, &req);
	if (rc == 0) {
		ctl->timebase_arg = arg;
		ctl->timebase_cb = timebase_cb;
	}
	return rc;
}

int
jack_transport_locate (jack_client_t *client, jack_nframes_t frame)
{
	jack_position_t pos;

	pos.frame = frame;
	pos.valid = 0;
	return jack_transport_request_new_pos (client, &pos);
}

jack_transport_state_t 
jack_transport_query (const jack_client_t *client, jack_position_t *pos)
{
	jack_control_t *ectl = client->engine;

	if (pos) {
		/* the guarded copy makes this function work in any
		 * thread 
		 */
		jack_transport_copy_position (&ectl->current_time, pos);
	}

	return ectl->transport_state;
}

int
jack_transport_reposition (jack_client_t *client, jack_position_t *pos)
{
	/* copy the input, to avoid modifying its contents */
	jack_position_t tmp = *pos;

	/* validate input */
	if (tmp.valid & ~JACK_POSITION_MASK) /* unknown field present? */
		return EINVAL;

	return jack_transport_request_new_pos (client, &tmp);
}

void  
jack_transport_start (jack_client_t *client)
{
	client->engine->transport_cmd = TransportCommandStart;
}

void
jack_transport_stop (jack_client_t *client)
{
	client->engine->transport_cmd = TransportCommandStop;
}


#ifdef OLD_TRANSPORT

/************* Compatibility with old transport API. *************/

int
jack_engine_takeover_timebase (jack_client_t *client)
{
	jack_error ("jack_engine_takeover_timebase() is no longer supported.");
	return ENOSYS;
}	

void
jack_get_transport_info (jack_client_t *client,
			 jack_transport_info_t *info)
{
	jack_control_t *ectl = client->engine;
	static int first_time = 1;

	if (first_time)
		jack_error ("jack_get_transport_info() is deprecated.");
	first_time = 0;

	/* check that this is the process thread */
	if (!pthread_equal(client->thread_id, pthread_self())) {
		jack_error("Invalid thread for jack_get_transport_info().");
		abort();		/* kill this client */
	}

	info->usecs = ectl->current_time.usecs;
	info->frame_rate = ectl->current_time.frame_rate;
	info->transport_state = ectl->transport_state;
	info->frame = ectl->current_time.frame;
	info->valid = (ectl->current_time.valid |
		       JackTransportState | JackTransportPosition);

	if (info->valid & JackTransportBBT) {
		info->bar = ectl->current_time.bar;
		info->beat = ectl->current_time.beat;
		info->tick = ectl->current_time.tick;
		info->bar_start_tick = ectl->current_time.bar_start_tick;
		info->beats_per_bar = ectl->current_time.beats_per_bar;
		info->beat_type = ectl->current_time.beat_type;
		info->ticks_per_beat = ectl->current_time.ticks_per_beat;
		info->beats_per_minute = ectl->current_time.beats_per_minute;
	}
}

void
jack_set_transport_info (jack_client_t *client,
			 jack_transport_info_t *info)
{
	static int first_time = 1;

	if (first_time)
		jack_error ("jack_set_transport_info() no longer supported.");
	first_time = 0;
}	

#endif /* OLD_TRANSPORT */
