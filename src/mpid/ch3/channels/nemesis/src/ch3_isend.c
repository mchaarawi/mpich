/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 *  (C) 2001 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

/* #include "mpidpre.h" */
#include "mpidi_ch3_impl.h"
#include "mpid_nem_impl.h"
#if defined (MPID_NEM_INLINE) && MPID_NEM_INLINE
#include "mpid_nem_inline.h"
#endif

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3_iSend
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int MPIDI_CH3_iSend (MPIDI_VC_t *vc, MPID_Request *sreq, void * hdr, MPIDI_msg_sz_t hdr_sz)
{
    int mpi_errno = MPI_SUCCESS;
    int again = 0;
    int in_cs = FALSE;
    MPIDI_STATE_DECL(MPID_STATE_MPIDI_CH3_ISEND);

    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_CH3_ISEND);

    if (vc->state == MPIDI_VC_STATE_MORIBUND) {
        sreq->status.MPI_ERROR = MPI_SUCCESS;
        MPIU_ERR_SET1(sreq->status.MPI_ERROR, MPI_ERR_PROC_FAIL_STOP, "**comm_fail", "**comm_fail %d", vc->pg_rank);
        MPIDI_CH3U_Request_complete(sreq);
        goto fn_fail;
    }

    if (VC_CH(vc)->iSendContig)
    {
        mpi_errno = VC_CH(vc)->iSendContig(vc, sreq, hdr, hdr_sz, NULL, 0);
        if(mpi_errno != MPI_SUCCESS) { MPIU_ERR_POP(mpi_errno); }
        goto fn_exit;
    }

    /* MPIU_Assert(VC_CH(vc)->is_local); */
    MPIU_Assert(hdr_sz <= sizeof(MPIDI_CH3_Pkt_t));

    /* This channel uses a fixed length header, the size of which
     * is the maximum of all possible packet headers */
    hdr_sz = sizeof(MPIDI_CH3_Pkt_t);
    MPIDI_DBG_Print_packet((MPIDI_CH3_Pkt_t*)hdr);

    MPIU_THREAD_CS_ENTER(MPIDCOMM,);
    in_cs = TRUE;

    if (MPIDI_CH3I_Sendq_empty(MPIDI_CH3I_shm_sendq))
    {
	MPIU_DBG_MSG_D (CH3_CHANNEL, VERBOSE, "iSend %d", (int) hdr_sz);
	mpi_errno = MPID_nem_mpich2_send_header (hdr, hdr_sz, vc, &again);
        if (mpi_errno) MPIU_ERR_POP (mpi_errno);
	if (again)
	{
	    goto enqueue_it;
	}
	else
	{
            int (*reqFn)(MPIDI_VC_t *, MPID_Request *, int *);

            reqFn = sreq->dev.OnDataAvail;
            if (!reqFn)
            {
                MPIU_Assert (MPIDI_Request_get_type (sreq) != MPIDI_REQUEST_TYPE_GET_RESP);
                MPIDI_CH3U_Request_complete (sreq);
            }
            else
            {
                int complete = 0;
                mpi_errno = reqFn (vc, sreq, &complete);
                if (mpi_errno) MPIU_ERR_POP (mpi_errno);
            }
	}
    }
    else
    {
	goto enqueue_it;
    }


 fn_exit:
    if (in_cs) {
        MPIU_THREAD_CS_EXIT(MPIDCOMM,);
    }

    MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3_ISEND);
    return mpi_errno;
 fn_fail:
    goto fn_exit;

 enqueue_it:
    MPIDI_DBG_PRINTF((55, FCNAME, "enqueuing"));

    sreq->dev.pending_pkt = *(MPIDI_CH3_PktGeneric_t *) hdr;
    sreq->dev.iov[0].MPID_IOV_BUF = (char *) &sreq->dev.pending_pkt;
    sreq->dev.iov[0].MPID_IOV_LEN = hdr_sz;
    sreq->dev.iov_count = 1;
    sreq->dev.iov_offset = 0;
    sreq->ch.noncontig = FALSE;
    sreq->ch.vc = vc;
    
    if (MPIDI_CH3I_Sendq_empty(MPIDI_CH3I_shm_sendq)) {
        MPIDI_CH3I_Sendq_enqueue(&MPIDI_CH3I_shm_sendq, sreq);
    } else {
        /* this is not the first send on the queue, enqueue it then
           check to see if we can send any now */
        MPIDI_CH3I_Sendq_enqueue(&MPIDI_CH3I_shm_sendq, sreq);
        mpi_errno = MPIDI_CH3I_Shm_send_progress();
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
    }
    
    goto fn_exit;
}

