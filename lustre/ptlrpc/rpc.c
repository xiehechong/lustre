/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define EXPORT_SYMTAB

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>

#define DEBUG_SUBSYSTEM S_RPC

#include <linux/obd_support.h>
#include <linux/lustre_net.h>

static ptl_handle_eq_t sent_pkt_eq, rcvd_rep_eq, 
        bulk_source_eq, bulk_sink_eq;


struct ptlrpc_request *ptlrpc_prep_req(struct ptlrpc_client *cl, 
                                       int opcode, int namelen, char *name,
                                       int tgtlen, char *tgt)
{
	struct ptlrpc_request *request;
	int rc;
	ENTRY; 

	OBD_ALLOC(request, sizeof(*request));
	if (!request) { 
		CERROR("request allocation out of memory\n");
		return NULL;
	}

	memset(request, 0, sizeof(*request));
	request->rq_xid = cl->cli_xid++;

	rc = cl->cli_req_pack(name, namelen, tgt, tgtlen,
			  &request->rq_reqhdr, &request->rq_req,
			  &request->rq_reqlen, &request->rq_reqbuf);
	if (rc) { 
		CERROR("cannot pack request %d\n", rc); 
		return NULL;
	}
	request->rq_reqhdr->opc = opcode;
	request->rq_reqhdr->seqno = request->rq_xid;

	EXIT;
	return request;
}

void ptlrpc_free_req(struct ptlrpc_request *request)
{
	OBD_FREE(request, sizeof(*request));
}

/* Abort this request and cleanup any resources associated with it. */
int ptl_abort_rpc(struct ptlrpc_request *request)
{
        /* First remove the MD for the reply; in theory, this means
         * that we can tear down the buffer safely. */
        PtlMEUnlink(request->rq_reply_me_h);
        PtlMDUnlink(request->rq_reply_md_h);

        if (request->rq_bulklen != 0) {
                PtlMEUnlink(request->rq_bulk_me_h);
                PtlMDUnlink(request->rq_bulk_md_h);
        }

        return 0;
}

int ptlrpc_queue_wait(struct ptlrpc_request *req, struct ptlrpc_client *cl)
{
	int rc;
        DECLARE_WAITQUEUE(wait, current);

	init_waitqueue_head(&req->rq_wait_for_rep);

	if (cl->cli_enqueue) {
		/* Local delivery */
                ENTRY;
		rc = cl->cli_enqueue(req); 
	} else {
		/* Remote delivery via portals. */
		req->rq_req_portal = cl->cli_request_portal;
		req->rq_reply_portal = cl->cli_reply_portal;
		rc = ptl_send_rpc(req, &cl->cli_server);
	}
	if (rc) { 
		CERROR("error %d, opcode %d\n", rc, 
		       req->rq_reqhdr->opc); 
		return -rc;
	}

        CDEBUG(0, "-- sleeping\n");
        add_wait_queue(&req->rq_wait_for_rep, &wait);
        while (req->rq_repbuf == NULL) {
                set_current_state(TASK_INTERRUPTIBLE);

                /* if this process really wants to die, let it go */
                if (sigismember(&(current->pending.signal), SIGKILL) ||
                    sigismember(&(current->pending.signal), SIGINT))
                        break;

                schedule();
        }
        remove_wait_queue(&req->rq_wait_for_rep, &wait);
        set_current_state(TASK_RUNNING);
        CDEBUG(0, "-- done\n");

        if (req->rq_repbuf == NULL) {
                /* We broke out because of a signal.  Clean up the dangling
                 * reply buffers! */
                ptl_abort_rpc(req);
                EXIT;
                return -EINTR;
        }

	rc = cl->cli_rep_unpack(req->rq_repbuf, req->rq_replen, &req->rq_rephdr,
                                &req->rq_rep);
	if (rc) {
		CERROR("unpack_rep failed: %d\n", rc);
		return rc;
	}
        CERROR("got rep %lld\n", req->rq_rephdr->seqno);
	if ( req->rq_rephdr->status == 0 )
                CDEBUG(0, "--> buf %p len %d status %d\n",
		       req->rq_repbuf, req->rq_replen, 
		       req->rq_rephdr->status); 

	EXIT;
	return 0;
}
/*
 *  Free the packet when it has gone out
 */
static int sent_packet_callback(ptl_event_t *ev, void *data)
{
        ENTRY;

        if (ev->type == PTL_EVENT_SENT) {
                OBD_FREE(ev->mem_desc.start, ev->mem_desc.length);
        } else { 
                // XXX make sure we understand all events, including ACK's
                CERROR("Unknown event %d\n", ev->type); 
                BUG();
        }

        EXIT;
        return 1;
}

/*
 * Wake up the thread waiting for the reply once it comes in.
 */
static int rcvd_reply_callback(ptl_event_t *ev, void *data)
{
        struct ptlrpc_request *rpc = ev->mem_desc.user_ptr;
        ENTRY;

        if (ev->type == PTL_EVENT_PUT) {
                rpc->rq_repbuf = ev->mem_desc.start + ev->offset;
                barrier();
                wake_up_interruptible(&rpc->rq_wait_for_rep);
        } else { 
                // XXX make sure we understand all events, including ACK's
                CERROR("Unknown event %d\n", ev->type); 
                BUG();
        }

        EXIT;
        return 1;
}

static int server_request_callback(ptl_event_t *ev, void *data)
{
        struct ptlrpc_service *service = data;
        int rc;

        if (ev->rlength != ev->mlength)
                CERROR("Warning: Possibly truncated rpc (%d/%d)\n",
                       ev->mlength, ev->rlength);

        /* The ME is unlinked when there is less than 1024 bytes free
         * on its MD.  This ensures we are always able to handle the rpc, 
         * although the 1024 value is a guess as to the size of a
         * large rpc (the known safe margin should be determined).
         *
         * NOTE: The portals API by default unlinks all MD's associated
         *       with an ME when it's unlinked.  For now, this behavior
         *       has been commented out of the portals library so the
         *       MD can be unlinked when its ref count drops to zero.
         *       A new MD and ME will then be created that use the same
         *       kmalloc()'ed memory and inserted at the ring tail.
         */

        service->srv_ref_count[service->srv_md_active]++;

        if (ev->offset >= (service->srv_buf_size - 1024)) {
                CDEBUG(D_INODE, "Unlinking ME %d\n", service->srv_me_active);

                rc = PtlMEUnlink(service->srv_me_h[service->srv_me_active]);
                service->srv_me_h[service->srv_me_active] = 0;

                if (rc != PTL_OK) {
                        CERROR("PtlMEUnlink failed - DROPPING soon: %d\n", rc);
                        return rc;
                }

                service->srv_me_active = NEXT_INDEX(service->srv_me_active,
                        service->srv_ring_length);

                if (service->srv_me_h[service->srv_me_active] == 0)
                        CERROR("All %d ring ME's are unlinked!\n",
                               service->srv_ring_length);
        }

        if (ev->type == PTL_EVENT_PUT) {
                wake_up(service->srv_wait_queue);
        } else {
                CERROR("Unexpected event type: %d\n", ev->type);
        }

        return 0;
}

static int bulk_source_callback(ptl_event_t *ev, void *data)
{
        struct ptlrpc_request *rpc = ev->mem_desc.user_ptr;

        ENTRY;

        if (ev->type == PTL_EVENT_SENT) {
                CDEBUG(D_NET, "got SENT event\n");
        } else if (ev->type == PTL_EVENT_ACK) {
                CDEBUG(D_NET, "got ACK event\n");
                rpc->rq_bulkbuf = NULL;
                wake_up_interruptible(&rpc->rq_wait_for_bulk);
        } else {
                CERROR("Unexpected event type!\n");
                BUG();
        }

        EXIT;
        return 1;
}

static int bulk_sink_callback(ptl_event_t *ev, void *data)
{
        struct ptlrpc_request *rpc = ev->mem_desc.user_ptr;

        ENTRY;

        if (ev->type == PTL_EVENT_PUT) {
                if (rpc->rq_bulkbuf != ev->mem_desc.start + ev->offset)
                        CERROR("bulkbuf != mem_desc -- why?\n");
                //wake_up_interruptible(&rpc->rq_wait_for_bulk);
        } else {
                CERROR("Unexpected event type!\n");
                BUG();
        }

        EXIT;
        return 1;
}

int ptl_send_buf(struct ptlrpc_request *request, struct lustre_peer *peer,
                 int portal)
{
        int rc;
        ptl_process_id_t remote_id;
        ptl_handle_md_t md_h;
        ptl_ack_req_t ack;

        switch (request->rq_type) {
        case PTLRPC_BULK:
                request->rq_req_md.start = request->rq_bulkbuf;
                request->rq_req_md.length = request->rq_bulklen;
                request->rq_req_md.eventq = bulk_source_eq;
                request->rq_req_md.threshold = 2; /* SENT and ACK events */
                ack = PTL_ACK_REQ;
                break;
        case PTLRPC_REQUEST:
                request->rq_req_md.start = request->rq_reqbuf;
                request->rq_req_md.length = request->rq_reqlen;
                request->rq_req_md.eventq = sent_pkt_eq;
                request->rq_req_md.threshold = 1;
                ack = PTL_NOACK_REQ;
                break;
        case PTLRPC_REPLY:
                request->rq_req_md.start = request->rq_repbuf;
                request->rq_req_md.length = request->rq_replen;
                request->rq_req_md.eventq = sent_pkt_eq;
                request->rq_req_md.threshold = 1;
                ack = PTL_NOACK_REQ;
                break;
        default:
                BUG();
        }
        request->rq_req_md.options = PTL_MD_OP_PUT;
        request->rq_req_md.user_ptr = request;

        rc = PtlMDBind(peer->peer_ni, request->rq_req_md, &md_h);
        if (rc != 0) {
                BUG();
                CERROR("PtlMDBind failed: %d\n", rc);
                return rc;
        }

        remote_id.addr_kind = PTL_ADDR_NID;
        remote_id.nid = peer->peer_nid;
        remote_id.pid = 0;

        CERROR("Sending %d bytes to portal %d, xid %d\n",
               request->rq_req_md.length, portal, request->rq_xid);

        rc = PtlPut(md_h, ack, remote_id, portal, 0, request->rq_xid, 0, 0);
        if (rc != PTL_OK) {
                BUG();
                CERROR("PtlPut(%d, %d, %d) failed: %d\n", remote_id.nid,
                       portal, request->rq_xid, rc);
                /* FIXME: tear down md */
        }

        return rc;
}

int ptl_send_rpc(struct ptlrpc_request *request, struct lustre_peer *peer)
{
        ptl_process_id_t local_id;
        int rc;
        char *repbuf;

        ENTRY;

        if (request->rq_replen == 0) {
                CERROR("request->rq_replen is 0!\n");
                EXIT;
                return -EINVAL;
        }

        /* request->rq_repbuf is set only when the reply comes in, in
         * client_packet_callback() */
        OBD_ALLOC(repbuf, request->rq_replen);
        if (!repbuf) { 
                EXIT;
                return -ENOMEM;
        }

        local_id.addr_kind = PTL_ADDR_GID;
        local_id.gid = PTL_ID_ANY;
        local_id.rid = PTL_ID_ANY;

        CERROR("sending req %d\n", request->rq_xid);
        rc = PtlMEAttach(peer->peer_ni, request->rq_reply_portal, local_id,
                         request->rq_xid, 0, PTL_UNLINK,
                         &request->rq_reply_me_h);
        if (rc != PTL_OK) {
                CERROR("PtlMEAttach failed: %d\n", rc);
                BUG();
                EXIT;
                goto cleanup;
        }

        request->rq_type = PTLRPC_REQUEST;
        request->rq_reply_md.start = repbuf;
        request->rq_reply_md.length = request->rq_replen;
        request->rq_reply_md.threshold = 1;
        request->rq_reply_md.options = PTL_MD_OP_PUT;
        request->rq_reply_md.user_ptr = request;
        request->rq_reply_md.eventq = rcvd_rep_eq;

        rc = PtlMDAttach(request->rq_reply_me_h, request->rq_reply_md,
                         PTL_UNLINK, &request->rq_reply_md_h);
        if (rc != PTL_OK) {
                CERROR("PtlMDAttach failed: %d\n", rc);
                BUG();
                EXIT;
                goto cleanup2;
        }

        if (request->rq_bulklen != 0) {
                rc = PtlMEAttach(peer->peer_ni, request->rq_bulk_portal,
                                 local_id, request->rq_xid, 0, PTL_UNLINK,
                                 &request->rq_bulk_me_h);
                if (rc != PTL_OK) {
                        CERROR("PtlMEAttach failed: %d\n", rc);
                        BUG();
                        EXIT;
                        goto cleanup3;
                }

                request->rq_bulk_md.start = request->rq_bulkbuf;
                request->rq_bulk_md.length = request->rq_bulklen;
                request->rq_bulk_md.threshold = 1;
                request->rq_bulk_md.options = PTL_MD_OP_PUT;
                request->rq_bulk_md.user_ptr = request;
                request->rq_bulk_md.eventq = bulk_sink_eq;

                rc = PtlMDAttach(request->rq_bulk_me_h,
                                 request->rq_bulk_md, PTL_UNLINK,
                                 &request->rq_bulk_md_h);
                if (rc != PTL_OK) {
                        CERROR("PtlMDAttach failed: %d\n", rc);
                        BUG();
                        EXIT;
                        goto cleanup4;
                }
        }

        return ptl_send_buf(request, peer, request->rq_req_portal);

 cleanup4:
        PtlMEUnlink(request->rq_bulk_me_h);
 cleanup3:
        PtlMDUnlink(request->rq_reply_md_h);
 cleanup2:
        PtlMEUnlink(request->rq_reply_me_h);
 cleanup:
        OBD_FREE(repbuf, request->rq_replen);

        return rc;
}

/* ptl_received_rpc() should be called by the sleeping process once
 * it finishes processing an event.  This ensures the ref count is
 * decremented and that the rpc ring buffer cycles properly.
 */ 
int ptl_received_rpc(struct ptlrpc_service *service) {
        int rc, index;

        index = service->srv_md_active;
        CDEBUG(D_INFO, "MD index=%d Ref Count=%d\n", index,
               service->srv_ref_count[index]);
        service->srv_ref_count[index]--;

        if ((service->srv_ref_count[index] <= 0) &&
            (service->srv_me_h[index] == 0)) {

                /* Replace the unlinked ME and MD */
                rc = PtlMEInsert(service->srv_me_h[service->srv_me_tail],
                                 service->srv_id, 0, ~0, PTL_RETAIN,
                                 PTL_INS_AFTER, &(service->srv_me_h[index]));
                CDEBUG(D_INFO, "Inserting new ME and MD in ring, rc %d\n", rc);
                service->srv_me_tail = index;
                service->srv_ref_count[index] = 0;
                
                if (rc != PTL_OK) {
                        CERROR("PtlMEInsert failed: %d\n", rc);
                        return rc;
                }

                service->srv_md[index].start        = service->srv_buf[index];
                service->srv_md[index].length       = service->srv_buf_size;
                service->srv_md[index].threshold    = PTL_MD_THRESH_INF;
                service->srv_md[index].options      = PTL_MD_OP_PUT;
                service->srv_md[index].user_ptr     = service;
                service->srv_md[index].eventq       = service->srv_eq_h;

                rc = PtlMDAttach(service->srv_me_h[index],
                                 service->srv_md[index],
                                 PTL_RETAIN, &(service->srv_md_h[index]));

                CDEBUG(D_INFO, "Attach MD in ring, rc %d\n", rc);
                if (rc != PTL_OK) {
                        /* XXX cleanup */
                        BUG();
                        CERROR("PtlMDAttach failed: %d\n", rc);
                        return rc;
                }

                service->srv_md_active =
                        NEXT_INDEX(index, service->srv_ring_length);
        } 
        
        return 0;
}

int rpc_register_service(struct ptlrpc_service *service, char *uuid)
{
        struct lustre_peer peer;
        int rc, i;

        rc = kportal_uuid_to_peer(uuid, &peer);
        if (rc != 0) {
                CERROR("Invalid uuid \"%s\"\n", uuid);
                return -EINVAL;
        }

        service->srv_ring_length = RPC_RING_LENGTH;
        service->srv_me_active = 0;
        service->srv_md_active = 0;

        service->srv_id.addr_kind = PTL_ADDR_GID;
        service->srv_id.gid = PTL_ID_ANY;
        service->srv_id.rid = PTL_ID_ANY;

        rc = PtlEQAlloc(peer.peer_ni, 128, server_request_callback,
                        service, &(service->srv_eq_h));

        if (rc != PTL_OK) {
                CERROR("PtlEQAlloc failed: %d\n", rc);
                return rc;
        }

        /* Attach the leading ME on which we build the ring */
        rc = PtlMEAttach(peer.peer_ni, service->srv_portal,
                         service->srv_id, 0, ~0, PTL_RETAIN,
                         &(service->srv_me_h[0]));

        if (rc != PTL_OK) {
                CERROR("PtlMEAttach failed: %d\n", rc);
                return rc;
        }

        for (i = 0; i < service->srv_ring_length; i++) {
                OBD_ALLOC(service->srv_buf[i], service->srv_buf_size);

                if (service->srv_buf[i] == NULL) {
                        CERROR("no memory\n");
                        return -ENOMEM;
                }

                /* Insert additional ME's to the ring */
                if (i > 0) {
                        rc = PtlMEInsert(service->srv_me_h[i-1],
                                         service->srv_id, 0, ~0, PTL_RETAIN,
                                         PTL_INS_AFTER,&(service->srv_me_h[i]));
                        service->srv_me_tail = i;

                        if (rc != PTL_OK) {
                                CERROR("PtlMEInsert failed: %d\n", rc);
                                return rc;
                        }
                }

                service->srv_ref_count[i] = 0;
                service->srv_md[i].start        = service->srv_buf[i];
                service->srv_md[i].length        = service->srv_buf_size;
                service->srv_md[i].threshold        = PTL_MD_THRESH_INF;
                service->srv_md[i].options        = PTL_MD_OP_PUT;
                service->srv_md[i].user_ptr        = service;
                service->srv_md[i].eventq        = service->srv_eq_h;

                rc = PtlMDAttach(service->srv_me_h[i], service->srv_md[i],
                                 PTL_RETAIN, &(service->srv_md_h[i]));

                if (rc != PTL_OK) {
                        /* cleanup */
                        CERROR("PtlMDAttach failed: %d\n", rc);
                        return rc;
                }
        }

        return 0;
}

int rpc_unregister_service(struct ptlrpc_service *service)
{
        int rc, i;

        for (i = 0; i < service->srv_ring_length; i++) {
                rc = PtlMDUnlink(service->srv_md_h[i]);
                if (rc)
                        CERROR("PtlMDUnlink failed: %d\n", rc);
        
                rc = PtlMEUnlink(service->srv_me_h[i]);
                if (rc)
                        CERROR("PtlMEUnlink failed: %d\n", rc);
        
                OBD_FREE(service->srv_buf[i], service->srv_buf_size);                
        }

        rc = PtlEQFree(service->srv_eq_h);
        if (rc)
                CERROR("PtlEQFree failed: %d\n", rc);

        return 0;
}

static int req_init_portals(void)
{
        int rc;
        const ptl_handle_ni_t *nip;
        ptl_handle_ni_t ni;

        nip = inter_module_get_request(LUSTRE_NAL "_ni", LUSTRE_NAL);
        if (nip == NULL) {
                CERROR("get_ni failed: is the NAL module loaded?\n");
                return -EIO;
        }
        ni = *nip;

        rc = PtlEQAlloc(ni, 128, sent_packet_callback, NULL, &sent_pkt_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 128, rcvd_reply_callback, NULL, &rcvd_rep_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 128, bulk_source_callback, NULL, &bulk_source_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        rc = PtlEQAlloc(ni, 128, bulk_sink_callback, NULL, &bulk_sink_eq);
        if (rc != PTL_OK)
                CERROR("PtlEQAlloc failed: %d\n", rc);

        return rc;
}

static int __init ptlrpc_init(void)
{
        return req_init_portals();
}

static void __exit ptlrpc_exit(void)
{
        PtlEQFree(sent_pkt_eq);
        PtlEQFree(rcvd_rep_eq);
        PtlEQFree(bulk_source_eq);
        PtlEQFree(bulk_sink_eq);

        inter_module_put(LUSTRE_NAL "_ni");

        return;
}

MODULE_AUTHOR("Peter J. Braam <braam@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Request Processor v1.0");
MODULE_LICENSE("GPL"); 

module_init(ptlrpc_init);
module_exit(ptlrpc_exit);
