/*
 * CDDL HEADER START
 *
 * Copyright(c) 2007-2009 Intel Corporation. All rights reserved.
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include "ixgbe_sw.h"

/* function prototypes */
static mblk_t *ixgbe_rx_bind(ixgbe_rx_data_t *, uint32_t, uint32_t);
static mblk_t *ixgbe_rx_copy(ixgbe_rx_data_t *, uint32_t, uint32_t);
static void ixgbe_rx_assoc_hcksum(mblk_t *, uint32_t);

#ifndef IXGBE_DEBUG
#pragma inline(ixgbe_rx_assoc_hcksum)
#endif

/*
 * ixgbe_rx_recycle - The call-back function to reclaim rx buffer.
 *
 * This function is called when an mp is freed by the user thru
 * freeb call (Only for mp constructed through desballoc call).
 * It returns back the freed buffer to the free list.
 */
void
ixgbe_rx_recycle(caddr_t arg)
{
	ixgbe_t *ixgbe;
	ixgbe_rx_ring_t *rx_ring;
	ixgbe_rx_data_t	*rx_data;
	rx_control_block_t *recycle_rcb;
	uint32_t free_index;
	uint32_t ref_cnt;

	recycle_rcb = (rx_control_block_t *)(uintptr_t)arg;
	rx_data = recycle_rcb->rx_data;
	rx_ring = rx_data->rx_ring;
	ixgbe = rx_ring->ixgbe;

	if (recycle_rcb->ref_cnt == 0) {
		/*
		 * This case only happens when rx buffers are being freed
		 * in ixgbe_stop() and freemsg() is called.
		 */
		return;
	}

	ASSERT(recycle_rcb->mp == NULL);

	/*
	 * Using the recycled data buffer to generate a new mblk
	 */
	recycle_rcb->mp = desballoc((unsigned char *)
	    recycle_rcb->rx_buf.address,
	    recycle_rcb->rx_buf.size,
	    0, &recycle_rcb->free_rtn);

	/*
	 * Put the recycled rx control block into free list
	 */
	mutex_enter(&rx_data->recycle_lock);

	free_index = rx_data->rcb_tail;
	ASSERT(rx_data->free_list[free_index] == NULL);

	rx_data->free_list[free_index] = recycle_rcb;
	rx_data->rcb_tail = NEXT_INDEX(free_index, 1, rx_data->free_list_size);

	mutex_exit(&rx_data->recycle_lock);

	/*
	 * The atomic operation on the number of the available rx control
	 * blocks in the free list is used to make the recycling mutual
	 * exclusive with the receiving.
	 */
	atomic_inc_32(&rx_data->rcb_free);
	ASSERT(rx_data->rcb_free <= rx_data->free_list_size);

	/*
	 * Considering the case that the interface is unplumbed
	 * and there are still some buffers held by the upper layer.
	 * When the buffer is returned back, we need to free it.
	 */
	ref_cnt = atomic_dec_32_nv(&recycle_rcb->ref_cnt);
	if (ref_cnt == 0) {
		if (recycle_rcb->mp != NULL) {
			freemsg(recycle_rcb->mp);
			recycle_rcb->mp = NULL;
		}

		ixgbe_free_dma_buffer(&recycle_rcb->rx_buf);

		mutex_enter(&ixgbe->rx_pending_lock);
		atomic_dec_32(&rx_data->rcb_pending);
		atomic_dec_32(&ixgbe->rcb_pending);

		/*
		 * When there is not any buffer belonging to this rx_data
		 * held by the upper layer, the rx_data can be freed.
		 */
		if ((rx_data->flag & IXGBE_RX_STOPPED) &&
		    (rx_data->rcb_pending == 0))
			ixgbe_free_rx_ring_data(rx_data);

		mutex_exit(&ixgbe->rx_pending_lock);
	}
}

/*
 * ixgbe_rx_copy - Use copy to process the received packet.
 *
 * This function will use bcopy to process the packet
 * and send the copied packet upstream.
 */
static mblk_t *
ixgbe_rx_copy(ixgbe_rx_data_t *rx_data, uint32_t index, uint32_t pkt_len)
{
	ixgbe_t *ixgbe;
	rx_control_block_t *current_rcb;
	mblk_t *mp;

	ixgbe = rx_data->rx_ring->ixgbe;
	current_rcb = rx_data->work_list[index];

	DMA_SYNC(&current_rcb->rx_buf, DDI_DMA_SYNC_FORKERNEL);

	if (ixgbe_check_dma_handle(current_rcb->rx_buf.dma_handle) !=
	    DDI_FM_OK) {
		ddi_fm_service_impact(ixgbe->dip, DDI_SERVICE_DEGRADED);
	}

	/*
	 * Allocate buffer to receive this packet
	 */
	mp = allocb(pkt_len + IPHDR_ALIGN_ROOM, 0);
	if (mp == NULL) {
		ixgbe_log(ixgbe, "ixgbe_rx_copy: allocate buffer failed");
		return (NULL);
	}

	/*
	 * Copy the data received into the new cluster
	 */
	mp->b_rptr += IPHDR_ALIGN_ROOM;
	bcopy(current_rcb->rx_buf.address, mp->b_rptr, pkt_len);
	mp->b_wptr = mp->b_rptr + pkt_len;

	return (mp);
}

/*
 * ixgbe_rx_bind - Use existing DMA buffer to build mblk for receiving.
 *
 * This function will use pre-bound DMA buffer to receive the packet
 * and build mblk that will be sent upstream.
 */
static mblk_t *
ixgbe_rx_bind(ixgbe_rx_data_t *rx_data, uint32_t index, uint32_t pkt_len)
{
	rx_control_block_t *current_rcb;
	rx_control_block_t *free_rcb;
	uint32_t free_index;
	mblk_t *mp;
	ixgbe_t	*ixgbe = rx_data->rx_ring->ixgbe;

	/*
	 * If the free list is empty, we cannot proceed to send
	 * the current DMA buffer upstream. We'll have to return
	 * and use bcopy to process the packet.
	 */
	if (ixgbe_atomic_reserve(&rx_data->rcb_free, 1) < 0)
		return (NULL);

	current_rcb = rx_data->work_list[index];
	/*
	 * If the mp of the rx control block is NULL, try to do
	 * desballoc again.
	 */
	if (current_rcb->mp == NULL) {
		current_rcb->mp = desballoc((unsigned char *)
		    current_rcb->rx_buf.address,
		    current_rcb->rx_buf.size,
		    0, &current_rcb->free_rtn);
		/*
		 * If it is failed to built a mblk using the current
		 * DMA buffer, we have to return and use bcopy to
		 * process the packet.
		 */
		if (current_rcb->mp == NULL) {
			atomic_inc_32(&rx_data->rcb_free);
			return (NULL);
		}
	}
	/*
	 * Sync up the data received
	 */
	DMA_SYNC(&current_rcb->rx_buf, DDI_DMA_SYNC_FORKERNEL);

	if (ixgbe_check_dma_handle(current_rcb->rx_buf.dma_handle) !=
	    DDI_FM_OK) {
		ddi_fm_service_impact(ixgbe->dip, DDI_SERVICE_DEGRADED);
	}

	mp = current_rcb->mp;
	current_rcb->mp = NULL;
	atomic_inc_32(&current_rcb->ref_cnt);

	mp->b_wptr = mp->b_rptr + pkt_len;
	mp->b_next = mp->b_cont = NULL;

	/*
	 * Strip off one free rx control block from the free list
	 */
	free_index = rx_data->rcb_head;
	free_rcb = rx_data->free_list[free_index];
	ASSERT(free_rcb != NULL);
	rx_data->free_list[free_index] = NULL;
	rx_data->rcb_head = NEXT_INDEX(free_index, 1, rx_data->free_list_size);

	/*
	 * Put the rx control block to the work list
	 */
	rx_data->work_list[index] = free_rcb;

	return (mp);
}

/*
 * ixgbe_rx_assoc_hcksum - Check the rx hardware checksum status and associate
 * the hcksum flags.
 */
static void
ixgbe_rx_assoc_hcksum(mblk_t *mp, uint32_t status_error)
{
	uint32_t hcksum_flags = 0;

	/*
	 * Check TCP/UDP checksum
	 */
	if ((status_error & IXGBE_RXD_STAT_L4CS) &&
	    !(status_error & IXGBE_RXDADV_ERR_TCPE))
		hcksum_flags |= HCK_FULLCKSUM | HCK_FULLCKSUM_OK;

	/*
	 * Check IP Checksum
	 */
	if ((status_error & IXGBE_RXD_STAT_IPCS) &&
	    !(status_error & IXGBE_RXDADV_ERR_IPE))
		hcksum_flags |= HCK_IPV4_HDRCKSUM;

	if (hcksum_flags != 0) {
		(void) hcksum_assoc(mp,
		    NULL, NULL, 0, 0, 0, 0, hcksum_flags, 0);
	}
}

/*
 * ixgbe_ring_rx - Receive the data of one ring.
 *
 * This function goes throught h/w descriptor in one specified rx ring,
 * receives the data if the descriptor status shows the data is ready.
 * It returns a chain of mblks containing the received data, to be
 * passed up to mac_rx().
 */
mblk_t *
ixgbe_ring_rx(ixgbe_rx_ring_t *rx_ring, int poll_bytes)
{
	union ixgbe_adv_rx_desc *current_rbd;
	rx_control_block_t *current_rcb;
	mblk_t *mp;
	mblk_t *mblk_head;
	mblk_t **mblk_tail;
	uint32_t rx_next;
	uint32_t rx_tail;
	uint32_t pkt_len;
	uint32_t status_error;
	uint32_t pkt_num;
	uint32_t received_bytes;
	ixgbe_t *ixgbe = rx_ring->ixgbe;
	ixgbe_rx_data_t *rx_data = rx_ring->rx_data;

	mblk_head = NULL;
	mblk_tail = &mblk_head;

	/*
	 * Sync the receive descriptors before accepting the packets
	 */
	DMA_SYNC(&rx_data->rbd_area, DDI_DMA_SYNC_FORKERNEL);

	if (ixgbe_check_dma_handle(rx_data->rbd_area.dma_handle) != DDI_FM_OK) {
		ddi_fm_service_impact(ixgbe->dip, DDI_SERVICE_DEGRADED);
	}

	/*
	 * Get the start point of rx bd ring which should be examined
	 * during this cycle.
	 */
	rx_next = rx_data->rbd_next;

	current_rbd = &rx_data->rbd_ring[rx_next];
	received_bytes = 0;
	pkt_num = 0;
	status_error = current_rbd->wb.upper.status_error;
	while (status_error & IXGBE_RXD_STAT_DD) {
		/*
		 * If adapter has found errors, but the error
		 * is hardware checksum error, this does not discard the
		 * packet: let upper layer compute the checksum;
		 * Otherwise discard the packet.
		 */
		if ((status_error & IXGBE_RXDADV_ERR_FRAME_ERR_MASK) ||
		    !(status_error & IXGBE_RXD_STAT_EOP)) {
			IXGBE_DEBUG_STAT(rx_ring->stat_frame_error);
			goto rx_discard;
		}

		IXGBE_DEBUG_STAT_COND(rx_ring->stat_cksum_error,
		    (status_error & IXGBE_RXDADV_ERR_TCPE) ||
		    (status_error & IXGBE_RXDADV_ERR_IPE));

		pkt_len = current_rbd->wb.upper.length;

		if ((poll_bytes != IXGBE_POLL_NULL) &&
		    ((received_bytes + pkt_len) > poll_bytes))
			break;

		received_bytes += pkt_len;

		mp = NULL;
		/*
		 * For packets with length more than the copy threshold,
		 * we'll first try to use the existing DMA buffer to build
		 * an mblk and send the mblk upstream.
		 *
		 * If the first method fails, or the packet length is less
		 * than the copy threshold, we'll allocate a new mblk and
		 * copy the packet data to the new mblk.
		 */
		if (pkt_len > ixgbe->rx_copy_thresh)
			mp = ixgbe_rx_bind(rx_data, rx_next, pkt_len);

		if (mp == NULL)
			mp = ixgbe_rx_copy(rx_data, rx_next, pkt_len);

		if (mp != NULL) {
			/*
			 * Check h/w checksum offload status
			 */
			if (ixgbe->rx_hcksum_enable)
				ixgbe_rx_assoc_hcksum(mp, status_error);

			*mblk_tail = mp;
			mblk_tail = &mp->b_next;
		}

rx_discard:
		/*
		 * Reset rx descriptor read bits
		 */
		current_rcb = rx_data->work_list[rx_next];
		current_rbd->read.pkt_addr = current_rcb->rx_buf.dma_address;
		current_rbd->read.hdr_addr = 0;

		rx_next = NEXT_INDEX(rx_next, 1, rx_data->ring_size);

		/*
		 * The receive function is in interrupt context, so here
		 * rx_limit_per_intr is used to avoid doing receiving too long
		 * per interrupt.
		 */
		if (++pkt_num > ixgbe->rx_limit_per_intr) {
			IXGBE_DEBUG_STAT(rx_ring->stat_exceed_pkt);
			break;
		}

		current_rbd = &rx_data->rbd_ring[rx_next];
		status_error = current_rbd->wb.upper.status_error;
	}

	DMA_SYNC(&rx_data->rbd_area, DDI_DMA_SYNC_FORDEV);

	rx_data->rbd_next = rx_next;

	/*
	 * Update the h/w tail accordingly
	 */
	rx_tail = PREV_INDEX(rx_next, 1, rx_data->ring_size);
	IXGBE_WRITE_REG(&ixgbe->hw, IXGBE_RDT(rx_ring->index), rx_tail);

	if (ixgbe_check_acc_handle(ixgbe->osdep.reg_handle) != DDI_FM_OK) {
		ddi_fm_service_impact(ixgbe->dip, DDI_SERVICE_DEGRADED);
	}

	return (mblk_head);
}

mblk_t *
ixgbe_ring_rx_poll(void *arg, int n_bytes)
{
	ixgbe_rx_ring_t *rx_ring = (ixgbe_rx_ring_t *)arg;
	mblk_t *mp = NULL;

	ASSERT(n_bytes >= 0);

	if (n_bytes == 0)
		return (mp);

	mutex_enter(&rx_ring->rx_lock);
	mp = ixgbe_ring_rx(rx_ring, n_bytes);
	mutex_exit(&rx_ring->rx_lock);

	return (mp);
}
