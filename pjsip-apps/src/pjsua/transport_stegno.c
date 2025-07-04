/* 
 * Copyright (C) 2008-2011 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2003-2008 Benny Prijono <benny@prijono.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */
#include "transport_stegno.h"
#include <pjmedia/endpoint.h>
#include <pj/assert.h>
#include <pj/pool.h>
#include <pj/log.h>
#include <pjmedia/rtp.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/msg.h>
#include <time.h>

#define THIS_FILE       "transport_stegno.c"
#define T(op)       do { \
                        pj_status_t status = op; \
                        if (status != PJ_SUCCESS) \
                            exit(0); \
                    } while (0)
#define MSG_NAME 81
#define RMSG_NAME 82
#define BUFSIZE 1024
#define MAX_LOG 10000

typedef struct {
    char op[8];
    struct timespec ts;
} time_log;
static time_log t_logs[MAX_LOG];
static int t_log_count = 0;


/* Transport functions prototypes */
static pj_status_t transport_get_info (pjmedia_transport *tp,
                                       pjmedia_transport_info *info);
static pj_status_t transport_attach2  (pjmedia_transport *tp,
                                       pjmedia_transport_attach_param *att_prm);
static void        transport_detach   (pjmedia_transport *tp,
                                       void *strm);
static pj_status_t transport_send_rtp( pjmedia_transport *tp,
                                       const void *pkt,
                                       pj_size_t size);
static pj_status_t transport_send_rtcp(pjmedia_transport *tp,
                                       const void *pkt,
                                       pj_size_t size);
static pj_status_t transport_send_rtcp2(pjmedia_transport *tp,
                                       const pj_sockaddr_t *addr,
                                       unsigned addr_len,
                                       const void *pkt,
                                       pj_size_t size);
static pj_status_t transport_media_create(pjmedia_transport *tp,
                                       pj_pool_t *sdp_pool,
                                       unsigned options,
                                       const pjmedia_sdp_session *rem_sdp,
                                       unsigned media_index);
static pj_status_t transport_encode_sdp(pjmedia_transport *tp,
                                       pj_pool_t *sdp_pool,
                                       pjmedia_sdp_session *local_sdp,
                                       const pjmedia_sdp_session *rem_sdp,
                                       unsigned media_index);
static pj_status_t transport_media_start (pjmedia_transport *tp,
                                       pj_pool_t *pool,
                                       const pjmedia_sdp_session *local_sdp,
                                       const pjmedia_sdp_session *rem_sdp,
                                       unsigned media_index);
static pj_status_t transport_media_stop(pjmedia_transport *tp);
static pj_status_t transport_simulate_lost(pjmedia_transport *tp,
                                       pjmedia_dir dir,
                                       unsigned pct_lost);
static pj_status_t transport_destroy  (pjmedia_transport *tp);


/* The transport operations */
static struct pjmedia_transport_op tp_stegno_op = 
{
    &transport_get_info,
    NULL,
    &transport_detach,
    &transport_send_rtp,
    &transport_send_rtcp,
    &transport_send_rtcp2,
    &transport_media_create,
    &transport_encode_sdp,
    &transport_media_start,
    &transport_media_stop,
    &transport_simulate_lost,
    &transport_destroy,
    &transport_attach2,
};


/* The transport adapter instance */
struct tp_stegno
{
    pjmedia_transport    base;
    pj_bool_t            del_base;

    pj_pool_t           *pool;

    /* Stream information. */
    void                *stream_user_data;
    void                *stream_ref;
    void               (*stream_rtp_cb)(void *user_data,
                                        void *pkt,
                                        pj_ssize_t);
    void               (*stream_rtp_cb2)(pjmedia_tp_cb_param *param);
    void               (*stream_rtcp_cb)(void *user_data,
                                         void *pkt,
                                         pj_ssize_t);


    /* Add your own member here.. */
    pjmedia_transport   *slave_tp;
};

static struct app
{
    pjmedia_rtp_session  rtp_sess;
    pj_bool_t            rtp_sess_init;
    int counter;
	int shmid;
    key_t key;
    char *shared_memory;
	int msgid;
	int rmsgid;
	pj_bool_t mod_payload;
	pj_bool_t mq_exist;
	pj_bool_t rmq_exist;
	pj_bool_t first_res;
} app;

/* message queue */
typedef struct msgbuf {
        unsigned long mtype;
        char mtext[BUFSIZE];      /* message text */
}MSGBUF;


static void adapter_on_destroy(void *arg);


/*
 * Create the adapter.
 */
PJ_DEF(pj_status_t) pjmedia_tp_stegno_create( pjmedia_endpt *endpt,
                                               const char *name,
                                               pjmedia_transport *transport,
                                               pj_bool_t del_base,
                                               pjmedia_transport **p_tp,
											   pj_bool_t mod_payload)
{
    pj_pool_t *pool;
    struct tp_stegno *adapter;
    PJ_LOG(3,(THIS_FILE, "inside pjmedia_tp_stegno_create"));

    if (name == NULL)
        name = "tpad%p";

    /* Create the pool and initialize the adapter structure */
    pool = pjmedia_endpt_create_pool(endpt, name, 512, 512);
    adapter = PJ_POOL_ZALLOC_T(pool, struct tp_stegno);
    adapter->pool = pool;
    pj_ansi_strxcpy(adapter->base.name, pool->obj_name, 
                    sizeof(adapter->base.name));
    adapter->base.type = (pjmedia_transport_type)
                         (PJMEDIA_TRANSPORT_TYPE_USER + 1);
    adapter->base.op = &tp_stegno_op;

    /* Save the transport as the slave transport */
    adapter->slave_tp = transport;
    adapter->del_base = del_base;

    app.counter = 0;
    // don't send to mq until hear first response from RX
    app.first_res = 0;
	// whether we need to modify payload
	app.mod_payload = mod_payload;


    /* Setup group lock handler for destroy and callback synchronization */
    if (transport && transport->grp_lock) {
        pj_grp_lock_t *grp_lock = transport->grp_lock;

        adapter->base.grp_lock = grp_lock;
        pj_grp_lock_add_ref(grp_lock);
        pj_grp_lock_add_handler(grp_lock, pool, adapter, &adapter_on_destroy);
    }

	
	/* message queue */
	if ((app.msgid = msgget((key_t)MSG_NAME, 0)) < 0) {
		app.mq_exist = 0;
		PJ_LOG(3, (THIS_FILE, "mq not exist" ));
	} else {
		app.mq_exist = 1;
		PJ_LOG(3, (THIS_FILE, "mq exist"));
		
	}
    if ((app.rmsgid = msgget((key_t)RMSG_NAME, 0)) < 0) {
        app.rmq_exist = 0;
        app.mod_payload = 0;
        PJ_LOG(3, (THIS_FILE, "rmq not exist"));
    } else {
        app.rmq_exist = 1;
        app.mod_payload = 1;
        PJ_LOG(3, (THIS_FILE, "rmq exist"));
    }

    
    /* Done */
    *p_tp = &adapter->base;
    return PJ_SUCCESS;
}


/*
 * get_info() is called to get the transport addresses to be put
 * in SDP c= line and a=rtcp line.
 */
static pj_status_t transport_get_info(pjmedia_transport *tp,
                                      pjmedia_transport_info *info)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;

    /* Since we don't have our own connection here, we just pass
     * this function to the slave transport.
     */
    return pjmedia_transport_get_info(adapter->slave_tp, info);
}


/* This is our RTP callback, that is called by the slave transport when it
 * receives RTP packet.
 */
static void transport_rtp_cb2(pjmedia_tp_cb_param *param)
{
    struct tp_stegno *adapter = (struct tp_stegno*)param->user_data;
    unsigned char *payload;

    pj_assert(adapter->stream_rtp_cb != NULL ||
              adapter->stream_rtp_cb2 != NULL);

    if (!app.mod_payload) {
    	see_rtp(param->pkt, param->size, payload);
    } else {
	app.first_res = 1;
    }

    /* Call stream's callback */
    if (adapter->stream_rtp_cb2) {
        pjmedia_tp_cb_param cbparam;
        
        pj_memcpy(&cbparam, param, sizeof(cbparam));
        cbparam.user_data = adapter->stream_user_data;
        adapter->stream_rtp_cb2(&cbparam);
    } else {
        adapter->stream_rtp_cb(adapter->stream_user_data, param->pkt,
                               param->size);
    }
}

/* This is our RTCP callback, that is called by the slave transport when it
 * receives RTCP packet.
 */
static void transport_rtcp_cb(void *user_data, void *pkt, pj_ssize_t size)
{
    struct tp_stegno *adapter = (struct tp_stegno*)user_data;

    pj_assert(adapter->stream_rtcp_cb != NULL);

    /* Call stream's callback */
    adapter->stream_rtcp_cb(adapter->stream_user_data, pkt, size);
}

/*
 * attach2() is called by stream to register callbacks that we should
 * call on receipt of RTP and RTCP packets.
 */
static pj_status_t transport_attach2(pjmedia_transport *tp,
                                     pjmedia_transport_attach_param *att_param)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;
    pj_status_t status;

    /* In this example, we will save the stream information and callbacks
     * to our structure, and we will register different RTP/RTCP callbacks
     * instead.
     */
    pj_assert(adapter->stream_user_data == NULL);
    adapter->stream_user_data = att_param->user_data;
    if (att_param->rtp_cb2) {
        adapter->stream_rtp_cb2 = att_param->rtp_cb2;
    } else {
        adapter->stream_rtp_cb = att_param->rtp_cb;
    }
    adapter->stream_rtcp_cb = att_param->rtcp_cb;
    adapter->stream_ref = att_param->stream;

    att_param->rtp_cb2 = &transport_rtp_cb2;
    att_param->rtp_cb = NULL;    
    att_param->rtcp_cb = &transport_rtcp_cb;
    att_param->user_data = adapter;
        
    status = pjmedia_transport_attach2(adapter->slave_tp, att_param);
    if (status != PJ_SUCCESS) {
        adapter->stream_user_data = NULL;
        adapter->stream_rtp_cb = NULL;
        adapter->stream_rtp_cb2 = NULL;
        adapter->stream_rtcp_cb = NULL;
        adapter->stream_ref = NULL;
        return status;
    }

    return PJ_SUCCESS;
}

static void dump_t_logs() {
    FILE *fp = fopen("pjsip_times.log", "w");
    if (!fp) return;
    for (int i = 0; i < t_log_count; ++i) {
        fprintf(fp, "%s %ld.%09ld\n", t_logs[i].op,
                t_logs[i].ts.tv_sec, t_logs[i].ts.tv_nsec);
    }
    fclose(fp);
}



/* 
 * detach() is called when the media is terminated, and the stream is 
 * to be disconnected from us.
 */
static void transport_detach(pjmedia_transport *tp, void *strm)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;
    
    PJ_UNUSED_ARG(strm);

    if (adapter->stream_user_data != NULL) {
        pjmedia_transport_detach(adapter->slave_tp, adapter);
        adapter->stream_user_data = NULL;
        adapter->stream_rtp_cb = NULL;
        adapter->stream_rtp_cb2 = NULL;
        adapter->stream_rtcp_cb = NULL;
        adapter->stream_ref = NULL;

        // remove message queue
        msgctl(app.msgid, IPC_RMID, 0);
        if (app.mod_payload) {
            msgctl(app.rmsgid, IPC_RMID, 0);
        }
        
        dump_t_logs();
    }
}

int see_rtp(const void *pkt, pj_size_t size, const void **payload)
{
    const pjmedia_rtp_hdr *rtp_header;
    int offset;
    int payloadlen;
    rtp_header = (pjmedia_rtp_hdr*)pkt;
    /* Payload is located right after header plus CSRC */
    offset = sizeof(pjmedia_rtp_hdr) + (rtp_header->cc * sizeof(pj_uint32_t));
    payload = (unsigned char *)(pkt + offset);
    payloadlen = size - offset;

    /* Remove payload padding if any */
    if (rtp_header->p && payloadlen > 0) {
        pj_uint8_t pad_len;

        pad_len = ((pj_uint8_t*)(*payload))[payloadlen - 1];
        if (pad_len <= payloadlen)
            payloadlen -= pad_len;
    }

    
    if (rtp_header->v == 2) {
       PJ_LOG(5, (THIS_FILE, "rtp version %u, payload type %u, seq %lu, ts %lu, ssrc=%lx, payload size %d, firstb %x, lastb %x",
            rtp_header->v, rtp_header->pt, (unsigned long)pj_ntohl(rtp_header->seq),
            (unsigned long) pj_ntohl(rtp_header->ts),
            (unsigned long) pj_ntohl(rtp_header->cc),
            payloadlen, payload[0], payload[payloadlen-1]));
    }

	MSGBUF msgbuf;
	MSGBUF rmsgbuf;

	if (app.mq_exist) {
		msgbuf.mtype = 1;
		memcpy(msgbuf.mtext, payload, payloadlen);
		msgsnd(app.msgid, &msgbuf, payloadlen, 0);

        if (t_log_count < MAX_LOG) {
            strcpy(t_logs[t_log_count].op, "txc");
            clock_gettime(CLOCK_MONOTONIC, &t_logs[t_log_count].ts);
            t_log_count++;
        }
	}
	if (app.rmq_exist) {
		/* if rmsq has more than one object, clear it */
		struct msqid_ds msqinfo;
		msgctl(app.rmsgid, IPC_STAT, &msqinfo);
		//PJ_LOG(3, (THIS_FILE, "rmsq %d", msqinfo.msg_qnum));
		for (int i = msqinfo.msg_qnum; i > 1; i--) {
			msgrcv(app.rmsgid, &rmsgbuf, payloadlen, 1, IPC_NOWAIT);
		}

		msgrcv(app.rmsgid, &rmsgbuf, payloadlen, 1, 0);

        if (t_log_count < MAX_LOG) {
            strcpy(t_logs[t_log_count].op, "rxc");
            clock_gettime(CLOCK_MONOTONIC, &t_logs[t_log_count].ts);
            t_log_count++;
        }

		memcpy(payload, rmsgbuf.mtext, payloadlen);
	}

	return payloadlen;
}

/*
 * send_rtp() is called to send RTP packet. The "pkt" and "size" argument 
 * contain both the RTP header and the payload.
 */
static pj_status_t transport_send_rtp( pjmedia_transport *tp,
                                       const void *pkt,
                                       pj_size_t size)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;

    /* You may do some processing to the RTP packet here if you want. */
    
    
    
    void *payload;
    int payloadlen;
    if (app.mod_payload && app.first_res) {
	    payloadlen = see_rtp(pkt, size, payload);
	//PJ_LOG(4, (THIS_FILE, "payloadlen %d", payloadlen));
    }
    

    

    /* Send the packet using the slave transport */
    return pjmedia_transport_send_rtp(adapter->slave_tp, pkt, size);
}


/*
 * send_rtcp() is called to send RTCP packet. The "pkt" and "size" argument
 * contain the RTCP packet.
 */
static pj_status_t transport_send_rtcp(pjmedia_transport *tp,
                                       const void *pkt,
                                       pj_size_t size)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;

    /* You may do some processing to the RTCP packet here if you want. */

    /* Send the packet using the slave transport */
    return pjmedia_transport_send_rtcp(adapter->slave_tp, pkt, size);
}


/*
 * This is another variant of send_rtcp(), with the alternate destination
 * address in the argument.
 */
static pj_status_t transport_send_rtcp2(pjmedia_transport *tp,
                                        const pj_sockaddr_t *addr,
                                        unsigned addr_len,
                                        const void *pkt,
                                        pj_size_t size)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;
    return pjmedia_transport_send_rtcp2(adapter->slave_tp, addr, addr_len, 
                                        pkt, size);
}

/*
 * The media_create() is called when the transport is about to be used for
 * a new call.
 */
static pj_status_t transport_media_create(pjmedia_transport *tp,
                                          pj_pool_t *sdp_pool,
                                          unsigned options,
                                          const pjmedia_sdp_session *rem_sdp,
                                          unsigned media_index)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;

    /* if "rem_sdp" is not NULL, it means we are UAS. You may do some
     * inspections on the incoming SDP to verify that the SDP is acceptable
     * for us. If the SDP is not acceptable, we can reject the SDP by 
     * returning non-PJ_SUCCESS.
     */
    if (rem_sdp) {
        /* Do your stuff.. */
        PJ_LOG(3,(THIS_FILE, "inside adapter create modified"));
    }

    /* Once we're done with our initialization, pass the call to the
     * slave transports to let it do it's own initialization too.
     */
    return pjmedia_transport_media_create(adapter->slave_tp, sdp_pool, options,
                                           rem_sdp, media_index);
}

/*
 * The encode_sdp() is called when we're about to send SDP to remote party,
 * either as SDP offer or as SDP answer.
 */
static pj_status_t transport_encode_sdp(pjmedia_transport *tp,
                                        pj_pool_t *sdp_pool,
                                        pjmedia_sdp_session *local_sdp,
                                        const pjmedia_sdp_session *rem_sdp,
                                        unsigned media_index)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;

    /* If "rem_sdp" is not NULL, it means we're encoding SDP answer. You may
     * do some more checking on the SDP's once again to make sure that
     * everything is okay before we send SDP.
     */
    if (rem_sdp) {
        /* Do checking stuffs here.. */
    }

    /* You may do anything to the local_sdp, e.g. adding new attributes, or
     * even modifying the SDP if you want.
     */
    if (1) {
        /* Say we add a proprietary attribute here.. */
        pjmedia_sdp_attr *my_attr;

        my_attr = PJ_POOL_ALLOC_T(sdp_pool, pjmedia_sdp_attr);
        pj_strdup2(sdp_pool, &my_attr->name, "X-adapter");
        pj_strdup2(sdp_pool, &my_attr->value, "some value");

        pjmedia_sdp_attr_add(&local_sdp->media[media_index]->attr_count,
                             local_sdp->media[media_index]->attr,
                             my_attr);
    }

    /* And then pass the call to slave transport to let it encode its 
     * information in the SDP. You may choose to call encode_sdp() to slave
     * first before adding your custom attributes if you want.
     */
    return pjmedia_transport_encode_sdp(adapter->slave_tp, sdp_pool, local_sdp,
                                        rem_sdp, media_index);
}

/*
 * The media_start() is called once both local and remote SDP have been
 * negotiated successfully, and the media is ready to start. Here we can start
 * committing our processing.
 */
static pj_status_t transport_media_start(pjmedia_transport *tp,
                                         pj_pool_t *pool,
                                         const pjmedia_sdp_session *local_sdp,
                                         const pjmedia_sdp_session *rem_sdp,
                                         unsigned media_index)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;

    /* Do something.. */

    /* And pass the call to the slave transport */
    return pjmedia_transport_media_start(adapter->slave_tp, pool, local_sdp,
                                         rem_sdp, media_index);
}

/*
 * The media_stop() is called when media has been stopped.
 */
static pj_status_t transport_media_stop(pjmedia_transport *tp)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;

    /* Do something.. */

    /* And pass the call to the slave transport */
    return pjmedia_transport_media_stop(adapter->slave_tp);
}

/*
 * simulate_lost() is called to simulate packet lost
 */
static pj_status_t transport_simulate_lost(pjmedia_transport *tp,
                                           pjmedia_dir dir,
                                           unsigned pct_lost)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;
    return pjmedia_transport_simulate_lost(adapter->slave_tp, dir, pct_lost);
}


static void adapter_on_destroy(void *arg)
{
    struct tp_stegno *adapter = (struct tp_stegno*)arg;

    pj_pool_release(adapter->pool);
}

/*
 * destroy() is called when the transport is no longer needed.
 */
static pj_status_t transport_destroy  (pjmedia_transport *tp)
{
    struct tp_stegno *adapter = (struct tp_stegno*)tp;

    /* Close the slave transport */
    if (adapter->del_base) {
        pjmedia_transport_close(adapter->slave_tp);
    }

    if (adapter->base.grp_lock) {
        pj_grp_lock_dec_ref(adapter->base.grp_lock);
    } else {
        adapter_on_destroy(tp);
    }

    return PJ_SUCCESS;
}





