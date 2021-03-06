#include "rdkafka_int.h"
#include "rdkafka_offset.h"
#include "rdkafka_topic.h"

int RD_TLS rd_kafka_yield_thread = 0;

void rd_kafka_yield (rd_kafka_t *rk) {
        rd_kafka_yield_thread = 1;
}


/**
 * Destroy a queue. refcnt must be at zero.
 */
void rd_kafka_q_destroy_final (rd_kafka_q_t *rkq) {

        mtx_lock(&rkq->rkq_lock);
	if (unlikely(rkq->rkq_qio != NULL)) {
		rd_free(rkq->rkq_qio);
		rkq->rkq_qio = NULL;
	}
        rd_kafka_q_fwd_set0(rkq, NULL, 0/*no-lock*/);
        rd_kafka_q_disable0(rkq, 0/*no-lock*/);
        rd_kafka_q_purge0(rkq, 0/*no-lock*/);
	assert(!rkq->rkq_fwdq);
        mtx_unlock(&rkq->rkq_lock);
	mtx_destroy(&rkq->rkq_lock);
	cnd_destroy(&rkq->rkq_cond);

        if (rkq->rkq_flags & RD_KAFKA_Q_F_ALLOCATED)
                rd_free(rkq);
}



/**
 * Initialize a queue.
 */
void rd_kafka_q_init (rd_kafka_q_t *rkq, rd_kafka_t *rk) {
        rd_kafka_q_reset(rkq);
	rkq->rkq_fwdq   = NULL;
        rkq->rkq_refcnt = 1;
        rkq->rkq_flags  = RD_KAFKA_Q_F_READY;
        rkq->rkq_rk     = rk;
	rkq->rkq_qio    = NULL;
	mtx_init(&rkq->rkq_lock, mtx_plain);
	cnd_init(&rkq->rkq_cond);
}


/**
 * Allocate a new queue and initialize it.
 */
rd_kafka_q_t *rd_kafka_q_new0 (rd_kafka_t *rk, const char *func, int line) {
        rd_kafka_q_t *rkq = rd_malloc(sizeof(*rkq));
        rd_kafka_q_init(rkq, rk);
        rkq->rkq_flags |= RD_KAFKA_Q_F_ALLOCATED;
#if ENABLE_DEVEL
	rd_snprintf(rkq->rkq_name, sizeof(rkq->rkq_name), "%s:%d", func, line);
#else
	rkq->rkq_name = func;
#endif
        return rkq;
}

/**
 * Set/clear forward queue.
 * Queue forwarding enables message routing inside rdkafka.
 * Typical use is to re-route all fetched messages for all partitions
 * to one single queue.
 *
 * All access to rkq_fwdq are protected by rkq_lock.
 */
void rd_kafka_q_fwd_set0 (rd_kafka_q_t *srcq, rd_kafka_q_t *destq,
                          int do_lock) {

        if (do_lock)
                mtx_lock(&srcq->rkq_lock);
	if (srcq->rkq_fwdq) {
		rd_kafka_q_destroy(srcq->rkq_fwdq);
		srcq->rkq_fwdq = NULL;
	}
	if (destq) {
		rd_kafka_q_keep(destq);

		/* If rkq has ops in queue, append them to fwdq's queue.
		 * This is an irreversible operation. */
                if (srcq->rkq_qlen > 0) {
			rd_dassert(destq->rkq_flags & RD_KAFKA_Q_F_READY);
			rd_kafka_q_concat(destq, srcq);
		}

		srcq->rkq_fwdq = destq;
	}
        if (do_lock)
                mtx_unlock(&srcq->rkq_lock);
}

/**
 * Purge all entries from a queue.
 */
int rd_kafka_q_purge0 (rd_kafka_q_t *rkq, int do_lock) {
	rd_kafka_op_t *rko, *next;
	TAILQ_HEAD(, rd_kafka_op_s) tmpq = TAILQ_HEAD_INITIALIZER(tmpq);
        int cnt = 0;

        if (do_lock)
                mtx_lock(&rkq->rkq_lock);

	if (rkq->rkq_fwdq) {
		cnt = rd_kafka_q_purge(rkq->rkq_fwdq);
                if (do_lock)
                        mtx_unlock(&rkq->rkq_lock);
		return cnt;
	}

	/* Move ops queue to tmpq to avoid lock-order issue
	 * by locks taken from rd_kafka_op_destroy(). */
	TAILQ_MOVE(&tmpq, &rkq->rkq_q, rko_link);

	/* Zero out queue */
        rd_kafka_q_reset(rkq);

        if (do_lock)
                mtx_unlock(&rkq->rkq_lock);

	/* Destroy the ops */
	next = TAILQ_FIRST(&tmpq);
	while ((rko = next)) {
		next = TAILQ_NEXT(next, rko_link);
		rd_kafka_op_destroy(rko);
                cnt++;
	}

        return cnt;
}


/**
 * Purge all entries from a queue with a rktp version smaller than `version`
 * This shaves off the head of the queue, up until the first rko with
 * a non-matching rktp or version.
 */
void rd_kafka_q_purge_toppar_version (rd_kafka_q_t *rkq,
                                      rd_kafka_toppar_t *rktp, int version) {
	rd_kafka_op_t *rko, *next;
	TAILQ_HEAD(, rd_kafka_op_s) tmpq = TAILQ_HEAD_INITIALIZER(tmpq);
        int32_t cnt = 0;
        int64_t size = 0;

	mtx_lock(&rkq->rkq_lock);

	if (rkq->rkq_fwdq) {
		rd_kafka_q_purge_toppar_version(rkq->rkq_fwdq, rktp, version);
		mtx_unlock(&rkq->rkq_lock);
		return;
	}

        /* Move ops to temporary queue and then destroy them from there
         * without locks to avoid lock-ordering problems in op_destroy() */
        while ((rko = TAILQ_FIRST(&rkq->rkq_q)) && rko->rko_rktp &&
               rd_kafka_toppar_s2i(rko->rko_rktp) == rktp &&
               rko->rko_version < version) {
                TAILQ_REMOVE(&rkq->rkq_q, rko, rko_link);
                TAILQ_INSERT_TAIL(&tmpq, rko, rko_link);
                cnt++;
                size += rko->rko_len;
        }


        rkq->rkq_qlen -= cnt;
        rkq->rkq_qsize -= size;
	mtx_unlock(&rkq->rkq_lock);

	next = TAILQ_FIRST(&tmpq);
	while ((rko = next)) {
		next = TAILQ_NEXT(next, rko_link);
		rd_kafka_op_destroy(rko);
	}
}


/**
 * Move 'cnt' entries from 'srcq' to 'dstq'.
 * If 'cnt' == -1 all entries will be moved.
 * Returns the number of entries moved.
 */
int rd_kafka_q_move_cnt (rd_kafka_q_t *dstq, rd_kafka_q_t *srcq,
			    int cnt, int do_locks) {
	rd_kafka_op_t *rko;
        int mcnt = 0;

        if (do_locks) {
		mtx_lock(&srcq->rkq_lock);
		mtx_lock(&dstq->rkq_lock);
	}

	if (!dstq->rkq_fwdq && !srcq->rkq_fwdq) {
		if (cnt > 0 && dstq->rkq_qlen == 0)
			rd_kafka_q_io_event(dstq);

		/* Optimization, if 'cnt' is equal/larger than all
		 * items of 'srcq' we can move the entire queue. */
		if (cnt == -1 ||
                    cnt >= (int)srcq->rkq_qlen) {
                        rd_dassert(TAILQ_EMPTY(&srcq->rkq_q) ||
                                   srcq->rkq_qlen > 0);
			TAILQ_CONCAT(&dstq->rkq_q, &srcq->rkq_q, rko_link);
			mcnt = srcq->rkq_qlen;
                        dstq->rkq_qlen += srcq->rkq_qlen;
                        dstq->rkq_qsize += srcq->rkq_qsize;
			rd_kafka_q_reset(srcq);
		} else {
			while (mcnt < cnt &&
			       (rko = TAILQ_FIRST(&srcq->rkq_q))) {
				TAILQ_REMOVE(&srcq->rkq_q, rko, rko_link);
				TAILQ_INSERT_TAIL(&dstq->rkq_q, rko, rko_link);
                                srcq->rkq_qlen--;
                                dstq->rkq_qlen++;
                                srcq->rkq_qsize -= rko->rko_len;
                                dstq->rkq_qsize += rko->rko_len;
				mcnt++;
			}
		}
	} else
		mcnt = rd_kafka_q_move_cnt(dstq->rkq_fwdq ? dstq->rkq_fwdq:dstq,
					   srcq->rkq_fwdq ? srcq->rkq_fwdq:srcq,
					   cnt, do_locks);

	if (do_locks) {
		mtx_unlock(&dstq->rkq_lock);
		mtx_unlock(&srcq->rkq_lock);
	}

	return mcnt;
}


/**
 * Filters out outdated ops.
 */
static RD_INLINE rd_kafka_op_t *rd_kafka_op_filter (rd_kafka_q_t *rkq,
						    rd_kafka_op_t *rko,
						    int version) {
        if (unlikely(!rko))
                return NULL;

        if (unlikely(rd_kafka_op_version_outdated(rko, version))) {
		rd_kafka_q_deq0(rkq, rko);
                rd_kafka_op_destroy(rko);
                return NULL;
        }

        return rko;
}



/**
 * Pop an op from a queue.
 *
 * Locality: any thread.
 */


/**
 * Serve q like rd_kafka_q_serve() until an op is found that can be returned
 * as an event to the application.
 *
 * @returns the first event:able op, or NULL on timeout.
 *
 * Locality: any thread
 */
rd_kafka_op_t *rd_kafka_q_pop_serve (rd_kafka_q_t *rkq, int timeout_ms,
				     int32_t version, int cb_type,
				     int (*callback) (rd_kafka_t *rk,
						      rd_kafka_op_t *rko,
						      int cb_type,
						      void *opaque),
				     void *opaque) {
	rd_kafka_op_t *rko;

	if (timeout_ms == RD_POLL_INFINITE)
		timeout_ms = INT_MAX;

	mtx_lock(&rkq->rkq_lock);

	if (!rkq->rkq_fwdq) {
                do {
                        /* Filter out outdated ops */
                        while ((rko = TAILQ_FIRST(&rkq->rkq_q)) &&
                               !(rko = rd_kafka_op_filter(rkq, rko, version)))
                                ;

                        if (rko) {
				int handled;

                                /* Proper versioned op */
                                rd_kafka_q_deq0(rkq, rko);

				/* Ops with callbacks are considered handled
				 * and we move on to the next op, if any.
				 * Ops w/o callbacks are returned immediately */
				if (callback) {
					handled = callback(rkq->rkq_rk, rko,
							   cb_type, opaque);
					if (handled) {
						rd_kafka_op_destroy(rko);
						rko = NULL;
					}
				} else
					handled = 0;

				if (!handled)
					break;
                        }

                        /* No op, wait for one */
			rd_ts_t pre = rd_clock();
			if (cnd_timedwait_ms(&rkq->rkq_cond,
					     &rkq->rkq_lock,
					     timeout_ms) ==
			    thrd_timedout) {
				mtx_unlock(&rkq->rkq_lock);
				return NULL;
			}
			/* Remove spent time */
			timeout_ms -= (int) (rd_clock()-pre) / 1000;
			if (timeout_ms < 0)
				timeout_ms = RD_POLL_NOWAIT;

		} while (timeout_ms != RD_POLL_NOWAIT);

                mtx_unlock(&rkq->rkq_lock);

	} else {
                rd_kafka_q_t *fwdq = rkq->rkq_fwdq;
                rd_kafka_q_keep(fwdq);
                /* Since the q_pop may block we need to release the parent
                 * queue's lock. */
                mtx_unlock(&rkq->rkq_lock);
		rko = rd_kafka_q_pop_serve(fwdq, timeout_ms, version,
					   cb_type, callback, opaque);
                rd_kafka_q_destroy(fwdq);
        }


	return rko;
}

rd_kafka_op_t *rd_kafka_q_pop (rd_kafka_q_t *rkq, int timeout_ms,
                               int32_t version) {
	return rd_kafka_q_pop_serve(rkq, timeout_ms, version, 0, NULL, NULL);
}


/**
 * Pop all available ops from a queue and call the provided 
 * callback for each op.
 * `max_cnt` limits the number of ops served, 0 = no limit.
 *
 * Returns the number of ops served.
 *
 * Locality: any thread.
 */
int rd_kafka_q_serve (rd_kafka_q_t *rkq, int timeout_ms,
                      int max_cnt, int cb_type,
                      int (*callback) (rd_kafka_t *rk, rd_kafka_op_t *rko,
                                       int cb_type, void *opaque),
                      void *opaque) {
        rd_kafka_t *rk = rkq->rkq_rk;
	rd_kafka_op_t *rko;
	rd_kafka_q_t localq;
        int cnt = 0;
        int handled = 0;

	mtx_lock(&rkq->rkq_lock);

        rd_dassert(TAILQ_EMPTY(&rkq->rkq_q) || rkq->rkq_qlen > 0);
	if (rkq->rkq_fwdq) {
                rd_kafka_q_t *fwdq = rkq->rkq_fwdq;
                int ret;
                rd_kafka_q_keep(fwdq);
                /* Since the q_pop may block we need to release the parent
                 * queue's lock. */
                mtx_unlock(&rkq->rkq_lock);
		ret = rd_kafka_q_serve(fwdq, timeout_ms, max_cnt,
                                       cb_type, callback, opaque);
                rd_kafka_q_destroy(fwdq);
		return ret;
	}

	if (timeout_ms == RD_POLL_INFINITE)
		timeout_ms = INT_MAX;

	/* Wait for op */
	while (!(rko = TAILQ_FIRST(&rkq->rkq_q)) && timeout_ms != 0) {
		if (cnd_timedwait_ms(&rkq->rkq_cond,
				     &rkq->rkq_lock,
				     timeout_ms) != thrd_success)
			break;

		timeout_ms = 0;
	}

	if (!rko) {
		mtx_unlock(&rkq->rkq_lock);
		return 0;
	}

	/* Move the first `max_cnt` ops. */
	rd_kafka_q_init(&localq, rkq->rkq_rk);
	rd_kafka_q_move_cnt(&localq, rkq, max_cnt == 0 ? -1/*all*/ : max_cnt,
			    0/*no-locks*/);

        mtx_unlock(&rkq->rkq_lock);

        rd_kafka_yield_thread = 0;

	/* Call callback for each op */
        while ((rko = TAILQ_FIRST(&localq.rkq_q))) {
		handled += callback(rk, rko, cb_type, opaque);
		rd_kafka_q_deq0(&localq, rko);
		rd_kafka_op_destroy(rko);
                cnt++;

                if (unlikely(rd_kafka_yield_thread)) {
                        /* Callback called rd_kafka_yield(), we must
                         * stop our callback dispatching and put the
                         * ops in localq back on the original queue head. */
                        if (!TAILQ_EMPTY(&localq.rkq_q))
                                rd_kafka_q_prepend(rkq, &localq);
                        break;
                }
	}

        /* Make sure no op was left unhandled. i.e.,
         * a consumer op ended up on the global queue. */
        rd_kafka_assert(NULL, handled == cnt);

	rd_kafka_q_destroy(&localq);

	return cnt;
}



void rd_kafka_message_destroy (rd_kafka_message_t *rkmessage) {
	rd_kafka_op_t *rko;

	if (likely((rko = (rd_kafka_op_t *)rkmessage->_private) != NULL))
		rd_kafka_op_destroy(rko);
	else {
		rd_kafka_msg_t *rkm = rd_kafka_message2msg(rkmessage);
		rd_kafka_msg_destroy(NULL, rkm);
	}
}


rd_kafka_message_t *rd_kafka_message_new (void) {
        rd_kafka_message_t *rkmessage;
        rkmessage = rd_calloc(1, sizeof(*rkmessage));
        return rkmessage;
}


static rd_kafka_message_t *
rd_kafka_message_setup (rd_kafka_op_t *rko, rd_kafka_message_t *rkmessage) {
	rd_kafka_itopic_t *rkt;
	rd_kafka_toppar_t *rktp = NULL;

	if (rko->rko_type == RD_KAFKA_OP_DR) {
		rkt = rd_kafka_topic_a2i(rko->rko_u.dr.rkt);
	} else {
		if (rko->rko_rktp) {
			rktp = rd_kafka_toppar_s2i(rko->rko_rktp);
			rkt = rktp->rktp_rkt;
		} else
			rkt = NULL;

		rkmessage->_private = rko;
	}


	if (!rkmessage->rkt && rkt)
		rkmessage->rkt = rd_kafka_topic_keep_a(rkt);

	if (rktp)
		rkmessage->partition = rktp->rktp_partition;

	if (!rkmessage->err)
		rkmessage->err = rko->rko_err;

	return rkmessage;
}



rd_kafka_message_t *rd_kafka_message_get_from_rkm (rd_kafka_op_t *rko,
						   rd_kafka_msg_t *rkm) {
	return rd_kafka_message_setup(rko, &rkm->rkm_rkmessage);
}

rd_kafka_message_t *rd_kafka_message_get (rd_kafka_op_t *rko) {
	rd_kafka_message_t *rkmessage;

	if (!rko)
		return rd_kafka_message_new(); /* empty */

	switch (rko->rko_type)
	{
	case RD_KAFKA_OP_FETCH:
		/* Use embedded rkmessage */
		rkmessage = &rko->rko_u.fetch.rkm.rkm_rkmessage;
		break;

	case RD_KAFKA_OP_ERR:
	case RD_KAFKA_OP_CONSUMER_ERR:
		rkmessage = &rko->rko_u.err.rkm.rkm_rkmessage;
		rkmessage->payload = rko->rko_u.err.errstr;
		rkmessage->offset  = rko->rko_u.err.offset;
		break;

	default:
		rd_kafka_assert(NULL, !*"unhandled optype");
		break;
	}

	return rd_kafka_message_setup(rko, rkmessage);
}


int64_t rd_kafka_message_timestamp (const rd_kafka_message_t *rkmessage,
				    rd_kafka_timestamp_type_t *tstype) {
	rd_kafka_msg_t *rkm;

	if (rkmessage->err) {
		*tstype = RD_KAFKA_TIMESTAMP_NOT_AVAILABLE;
		return -1;
	}

	rkm = rd_kafka_message2msg((rd_kafka_message_t *)rkmessage);

	*tstype = rkm->rkm_tstype;

	return rkm->rkm_timestamp;
}



/**
 * Populate 'rkmessages' array with messages from 'rkq'.
 * If 'auto_commit' is set, each message's offset will be committed
 * to the offset store for that toppar.
 *
 * Returns the number of messages added.
 */

int rd_kafka_q_serve_rkmessages (rd_kafka_q_t *rkq, int timeout_ms,
                                 rd_kafka_message_t **rkmessages,
                                 size_t rkmessages_size) {
	unsigned int cnt = 0;
        TAILQ_HEAD(, rd_kafka_op_s) tmpq = TAILQ_HEAD_INITIALIZER(tmpq);
        rd_kafka_op_t *rko, *next;
        rd_kafka_t *rk = rkq->rkq_rk;

	mtx_lock(&rkq->rkq_lock);
	if (rkq->rkq_fwdq) {
                rd_kafka_q_t *fwdq = rkq->rkq_fwdq;
                rd_kafka_q_keep(fwdq);
                /* Since the q_pop may block we need to release the parent
                 * queue's lock. */
                mtx_unlock(&rkq->rkq_lock);
		cnt = rd_kafka_q_serve_rkmessages(fwdq, timeout_ms,
						  rkmessages, rkmessages_size);
                rd_kafka_q_destroy(fwdq);
		return cnt;
	}
        mtx_unlock(&rkq->rkq_lock);

	while (cnt < rkmessages_size) {

                mtx_lock(&rkq->rkq_lock);

		while (!(rko = TAILQ_FIRST(&rkq->rkq_q))) {
			if (cnd_timedwait_ms(&rkq->rkq_cond, &rkq->rkq_lock,
                                             timeout_ms) == thrd_timedout)
				break;
		}

		if (!rko) {
                        mtx_unlock(&rkq->rkq_lock);
			break; /* Timed out */
                }

		rd_kafka_q_deq0(rkq, rko);

                mtx_unlock(&rkq->rkq_lock);

		if (rd_kafka_op_version_outdated(rko, 0)) {
                        /* Outdated op, put on discard queue */
                        TAILQ_INSERT_TAIL(&tmpq, rko, rko_link);
                        continue;
                }

                /* Serve callbacks */
                if (rd_kafka_poll_cb(rk, rko, _Q_CB_CONSUMER, NULL)) {
                        /* Callback served, rko is done, put on discard queue */
                        TAILQ_INSERT_TAIL(&tmpq, rko, rko_link);
                        continue;
                }

		/* Auto-commit offset, if enabled. */
		if (!rko->rko_err && rko->rko_type == RD_KAFKA_OP_FETCH) {
                        rd_kafka_toppar_t *rktp;
                        rktp = rd_kafka_toppar_s2i(rko->rko_rktp);
			rd_kafka_toppar_lock(rktp);
			rktp->rktp_app_offset = rko->rko_u.fetch.rkm.rkm_offset+1;
                        if (rktp->rktp_cgrp &&
			    rk->rk_conf.enable_auto_offset_store)
                                rd_kafka_offset_store0(rktp,
						       rktp->rktp_app_offset,
                                                       0/* no lock */);
			rd_kafka_toppar_unlock(rktp);
                }

		/* Get rkmessage from rko and append to array. */
		rkmessages[cnt++] = rd_kafka_message_get(rko);
	}

        /* Discard non-desired and already handled ops */
        next = TAILQ_FIRST(&tmpq);
        while (next) {
                rko = next;
                next = TAILQ_NEXT(next, rko_link);
                rd_kafka_op_destroy(rko);
        }


	return cnt;
}



void rd_kafka_queue_destroy (rd_kafka_queue_t *rkqu) {
	rd_kafka_q_disable(rkqu->rkqu_q);
	rd_kafka_q_destroy(rkqu->rkqu_q);
	rd_free(rkqu);
}

rd_kafka_queue_t *rd_kafka_queue_new0 (rd_kafka_t *rk, rd_kafka_q_t *rkq) {
	rd_kafka_queue_t *rkqu;

	rkqu = rd_calloc(1, sizeof(*rkqu));

	rkqu->rkqu_q = rkq;
	rd_kafka_q_keep(rkq);

        rkqu->rkqu_rk = rk;

	return rkqu;
}


rd_kafka_queue_t *rd_kafka_queue_new (rd_kafka_t *rk) {
	rd_kafka_q_t *rkq;
	rd_kafka_queue_t *rkqu;

	rkq = rd_kafka_q_new(rk);
	rkqu = rd_kafka_queue_new0(rk, rkq);
	rd_kafka_q_destroy(rkq); /* Loose refcount from q_new, one is held
				  * by queue_new0 */
	return rkqu;
}


rd_kafka_queue_t *rd_kafka_queue_get_main (rd_kafka_t *rk) {
	return rd_kafka_queue_new0(rk, rk->rk_rep);
}


rd_kafka_queue_t *rd_kafka_queue_get_consumer (rd_kafka_t *rk) {
	if (!rk->rk_cgrp)
		return NULL;
	return rd_kafka_queue_new0(rk, rk->rk_cgrp->rkcg_q);
}

void rd_kafka_queue_forward (rd_kafka_queue_t *src, rd_kafka_queue_t *dst) {
	rd_kafka_q_fwd_set(src->rkqu_q, dst ? dst->rkqu_q : NULL);
}


size_t rd_kafka_queue_length (rd_kafka_queue_t *rkqu) {
	return (size_t)rd_kafka_q_len(rkqu->rkqu_q);
}

void rd_kafka_queue_io_event_enable (rd_kafka_queue_t *rkqu, int fd,
				     const void *payload, size_t size) {
	rd_kafka_q_t *rkq = rkqu->rkqu_q;
	struct rd_kafka_q_io *qio;

	if (fd != -1) {
		qio = rd_malloc(sizeof(*qio) + size);
		qio->fd = fd;
		qio->size = size;
		qio->payload = (void *)(qio+1);
		memcpy(qio->payload, payload, size);
	}

	mtx_lock(&rkq->rkq_lock);
	if (rkq->rkq_qio) {
		rd_free(rkq->rkq_qio);
		rkq->rkq_qio = NULL;
	}

	if (fd != -1) {
		rkq->rkq_qio = qio;
	}

	mtx_unlock(&rkq->rkq_lock);
}


/**
 * Helper: wait for single op on 'rkq', and return its error,
 * or .._TIMED_OUT on timeout.
 */
rd_kafka_resp_err_t rd_kafka_q_wait_result (rd_kafka_q_t *rkq, int timeout_ms) {
        rd_kafka_op_t *rko;
        rd_kafka_resp_err_t err;

        rko = rd_kafka_q_pop(rkq, timeout_ms, 0);
        if (!rko)
                err = RD_KAFKA_RESP_ERR__TIMED_OUT;
        else {
                err = rko->rko_err;
                rd_kafka_op_destroy(rko);
        }

        return err;
}


/**
 * @brief Convert relative to absolute offsets and also purge any messages
 *        that are older than \p min_offset.
 * @remark Error ops with ERR__NOT_IMPLEMENTED will not be purged since
 *         they are used to indicate unknnown compression codecs and compressed
 *         messagesets may have a starting offset lower than what we requested.
 * @remark \p rkq locking is not performed (caller's responsibility)
 * @remark Must NOT be used on fwdq.
 */
void rd_kafka_q_fix_offsets (rd_kafka_q_t *rkq, int64_t min_offset,
			     int64_t base_offset) {
	rd_kafka_op_t *rko, *next;
	int     adj_len  = 0;
	int64_t adj_size = 0;

	rd_kafka_assert(NULL, !rkq->rkq_fwdq);

	next = TAILQ_FIRST(&rkq->rkq_q);
	while ((rko = next)) {
		next = TAILQ_NEXT(next, rko_link);

		if (unlikely(rko->rko_type != RD_KAFKA_OP_FETCH))
			continue;

		if (rko->rko_u.fetch.rkm.rkm_offset < min_offset &&
		    rko->rko_err != RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED) {
			adj_len++;
			adj_size += rko->rko_len;
			TAILQ_REMOVE(&rkq->rkq_q, rko, rko_link);
			rd_kafka_op_destroy(rko);
			continue;
		}

		rko->rko_u.fetch.rkm.rkm_offset += base_offset;
	}


	rkq->rkq_qlen  -= adj_len;
	rkq->rkq_qsize -= adj_size;
}
