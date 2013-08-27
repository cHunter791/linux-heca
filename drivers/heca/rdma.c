#include <linux/list.h>

#include "ioctl.h"
#include "trace.h"
#include "conn.h"
#include "base.h"
#include "ops.h"
#include "sysfs.h"
#include "rdma.h"

static int connect_client(struct rdma_cm_id *id)
{
        int r;
        struct rdma_conn_param param;

        memset(&param, 0, sizeof(struct rdma_conn_param));
        param.responder_resources = 1;
        param.initiator_depth = 1;
        param.retry_count = 10;

        r = rdma_connect(id, &param);
        if (r)
                heca_printk(KERN_ERR "Failed rdma_connect: %d", r);

        return r;
}

static inline void setup_IB_attr(struct ib_qp_init_attr *attr,
                struct ib_device_attr dev_attr)
{
        attr->cap.max_send_wr = min(dev_attr.max_qp_wr, IB_MAX_CAP_SCQ);
        attr->cap.max_recv_wr = min(dev_attr.max_qp_wr, IB_MAX_CAP_RCQ);
        attr->cap.max_send_sge = min(dev_attr.max_sge, IB_MAX_SEND_SGE);
        attr->cap.max_recv_sge = min(dev_attr.max_sge, IB_MAX_RECV_SGE);
}

static inline void setup_IW_attr(struct ib_qp_init_attr *attr,
                struct ib_device_attr dev_attr)
{
        attr->cap.max_send_wr = min(dev_attr.max_qp_wr, IW_MAX_CAP_SCQ);
        attr->cap.max_recv_wr = min(dev_attr.max_qp_wr, IW_MAX_CAP_RCQ);
        attr->cap.max_send_sge = min(dev_attr.max_sge, IW_MAX_SEND_SGE);
        attr->cap.max_recv_sge = min(dev_attr.max_sge, IW_MAX_RECV_SGE);
}

static inline int setup_qp_attr(struct heca_connection *conn)
{
        struct ib_qp_init_attr * attr = &conn->qp_attr;
        int ret = -1;
        struct ib_device_attr dev_attr;

        if (ib_query_device(conn->cm_id->device, &dev_attr)) {
                heca_printk("Query device failed for %s",
                                conn->cm_id->device->name);
                goto out;
        }
        attr->sq_sig_type = IB_SIGNAL_ALL_WR;
        attr->qp_type = IB_QPT_RC;
        attr->port_num = conn->cm_id->port_num;
        attr->qp_context = (void *) conn;
        switch (rdma_node_get_transport(conn->cm_id->device->node_type)) {
        case RDMA_TRANSPORT_IB:
                setup_IB_attr(attr, dev_attr);
                break;
        case RDMA_TRANSPORT_IWARP:
                setup_IW_attr(attr, dev_attr);
                break;
        default:
                return -1;
        }

        ret = 0;
out:
        return ret;
}

static int create_qp(struct heca_connection *conn)
{
        int ret = -1;
        struct ib_qp_init_attr * attr;

        attr = &conn->qp_attr;

        if (unlikely(!conn->cm_id))
                goto exit;

        if (unlikely(!conn->pd))
                goto exit;

        ret = rdma_create_qp(conn->cm_id, conn->pd, attr);

exit:
        return ret;
}

static int setup_qp(struct heca_connection *conn)
{
        int ret = 0;

        INIT_WORK(&conn->send_work, send_cq_handle_work);
        INIT_WORK(&conn->recv_work, recv_cq_handle_work);

        conn->qp_attr.send_cq = ib_create_cq(conn->cm_id->device,
                        send_cq_handle, heca_cq_event_handler, (void *) conn,
                        conn->qp_attr.cap.max_send_wr, 0);
        if (IS_ERR(conn->qp_attr.send_cq)) {
                heca_printk(KERN_ERR "Cannot create cq");
                goto err1;
        }

        if (ib_req_notify_cq(conn->qp_attr.send_cq, IB_CQ_NEXT_COMP)) {
                heca_printk(KERN_ERR "Cannot notify cq");
                goto err2;
        }

        conn->qp_attr.recv_cq = ib_create_cq(conn->cm_id->device,
                        recv_cq_handle, heca_cq_event_handler, (void *) conn,
                        conn->qp_attr.cap.max_recv_wr,0);
        if (IS_ERR(conn->qp_attr.recv_cq)) {
                heca_printk(KERN_ERR "Cannot create cq");
                goto err3;
        }

        if (ib_req_notify_cq(conn->qp_attr.recv_cq, IB_CQ_NEXT_COMP)) {
                heca_printk(KERN_ERR "Cannot notify cq");
                goto err4;
        }

        if (create_qp(conn)) {
                goto err5;
                heca_printk(KERN_ERR "QP not created --> Cancelled");
        }

        return ret;

err5: ret++;
err4: ret++;
      ib_destroy_cq(conn->qp_attr.recv_cq);
err3: ret++;
err2: ret++;
      ib_destroy_cq(conn->qp_attr.send_cq);
err1: ret++;
      heca_printk(KERN_ERR "Could not setup the qp, error %d occurred", ret);
      return ret;
}

static void init_tx_wr(struct tx_buffer_element *tx_ele, u32 lkey, int id)
{
        BUG_ON(!tx_ele);
        BUG_ON(!tx_ele->wrk_req);
        BUG_ON(!tx_ele->wrk_req->wr_ele);

        tx_ele->wrk_req->wr_ele->wr.wr_id = (u64) id;
        tx_ele->wrk_req->wr_ele->wr.opcode = IB_WR_SEND;
        tx_ele->wrk_req->wr_ele->wr.send_flags = IB_SEND_SIGNALED;
        tx_ele->wrk_req->wr_ele->wr.num_sge = 1;
        tx_ele->wrk_req->wr_ele->wr.sg_list =
                (struct ib_sge *) &tx_ele->wrk_req->wr_ele->sg;

        tx_ele->wrk_req->wr_ele->sg.addr = tx_ele->heca_dma.addr;
        tx_ele->wrk_req->wr_ele->sg.length = tx_ele->heca_dma.size;
        tx_ele->wrk_req->wr_ele->sg.lkey = lkey;

        tx_ele->wrk_req->wr_ele->wr.next = NULL;
}

static void init_reply_wr(struct heca_reply_work_request *rwr, u64 msg_addr,
                u32 lkey, int id)
{
        struct ib_sge *reply_sge;

        BUG_ON(!rwr);
        BUG_ON(!rwr->hwr_ele);

        reply_sge = &rwr->hwr_ele->sg;
        BUG_ON(!reply_sge);
        reply_sge->addr = msg_addr;
        reply_sge->length = sizeof(struct heca_message);
        reply_sge->lkey = lkey;

        rwr->hwr_ele->heca_dma.addr = msg_addr;
        rwr->hwr_ele->wr.next = NULL;
        rwr->hwr_ele->wr.num_sge = 1;
        rwr->hwr_ele->wr.send_flags = IB_SEND_SIGNALED;
        rwr->hwr_ele->wr.opcode = IB_WR_SEND;
        rwr->hwr_ele->wr.sg_list = (struct ib_sge *) &rwr->hwr_ele->sg;
        rwr->hwr_ele->wr.wr_id = id;
}

static void init_page_wr(struct heca_reply_work_request *rwr, u32 lkey, int id)
{
        rwr->page_sgl.addr = 0;
        rwr->page_sgl.length = PAGE_SIZE;
        rwr->page_sgl.lkey = lkey;

        rwr->wr.next = &rwr->hwr_ele->wr;
        rwr->wr.sg_list = (struct ib_sge *) &rwr->page_sgl;
        rwr->wr.send_flags = IB_SEND_SIGNALED;
        rwr->wr.opcode = IB_WR_RDMA_WRITE;
        rwr->wr.num_sge = 1;
        rwr->wr.wr_id = id;
}

static void init_tx_ele(struct tx_buffer_element *tx_ele,
                struct heca_connection *conn, int id)
{
        BUG_ON(!tx_ele);
        tx_ele->id = id;
        init_tx_wr(tx_ele, conn->mr->lkey, tx_ele->id);
        init_reply_wr(tx_ele->reply_work_req, tx_ele->heca_dma.addr,
                        conn->mr->lkey, tx_ele->id);
        BUG_ON(!conn->mr);
        init_page_wr(tx_ele->reply_work_req, conn->mr->lkey, tx_ele->id);
        tx_ele->hmsg_buffer->dest_id = conn->mr->rkey;
        tx_ele->hmsg_buffer->offset = tx_ele->id;
}

static void destroy_tx_buffer(struct heca_connection *conn)
{
        int i;
        struct tx_buffer_element *tx_buf = conn->tx_buffer.tx_buf;

        if (!tx_buf)
                return;
        cancel_work_sync(&conn->delayed_request_flush_work);

        for (i = 0; i < conn->tx_buffer.len; ++i) {
                if (tx_buf[i].heca_dma.addr) {
                        ib_dma_unmap_single(conn->cm_id->device,
                                        tx_buf[i].heca_dma.addr,
                                        tx_buf[i].heca_dma.size,
                                        tx_buf[i].heca_dma.dir);
                }
                kfree(tx_buf[i].hmsg_buffer);
                kfree(tx_buf[i].wrk_req->wr_ele);
                kfree(tx_buf[i].wrk_req);
        }

        kfree(tx_buf);
        conn->tx_buffer.tx_buf = 0;
}

static void destroy_rx_buffer(struct heca_connection *conn)
{
        int i;
        struct rx_buffer_element *rx = conn->rx_buffer.rx_buf;

        if (!rx)
                return;

        for (i = 0; i < conn->rx_buffer.len; ++i) {
                if (rx[i].heca_dma.addr) {
                        ib_dma_unmap_single(conn->cm_id->device,
                                        rx[i].heca_dma.addr,
                                        rx[i].heca_dma.size,
                                        rx[i].heca_dma.dir);
                }
                kfree(rx[i].hmsg_buffer);
                kfree(rx[i].recv_wrk_rq_ele);
        }
        kfree(rx);
        conn->rx_buffer.rx_buf = 0;
}

static int create_tx_buffer(struct heca_connection *conn)
{
        int i, ret = 0;
        struct tx_buffer_element *tx_buff_e;

        BUG_ON(!conn);
        BUG_ON(IS_ERR(conn->cm_id));
        BUG_ON(!conn->cm_id->device);
        might_sleep();

        conn->tx_buffer.len = get_nb_tx_buff_elements(conn);
        tx_buff_e = kzalloc((sizeof(struct tx_buffer_element) *
                                conn->tx_buffer.len), GFP_KERNEL);
        if (unlikely(!tx_buff_e)) {
                heca_printk(KERN_ERR "Can't allocate memory");
                return -ENOMEM;
        }
        conn->tx_buffer.tx_buf = tx_buff_e;

        for (i = 0; i < conn->tx_buffer.len; ++i) {
                tx_buff_e[i].hmsg_buffer = kzalloc(sizeof(struct heca_message),
                                GFP_KERNEL);
                if (!tx_buff_e[i].hmsg_buffer) {
                        heca_printk(KERN_ERR "Failed to allocate .heca_buf");
                        ret = -ENOMEM;
                        goto err;
                }

                tx_buff_e[i].heca_dma.dir = DMA_TO_DEVICE;
                tx_buff_e[i].heca_dma.size = sizeof(struct heca_message);
                tx_buff_e[i].heca_dma.addr = ib_dma_map_single(
                                conn->cm_id->device,
                                tx_buff_e[i].hmsg_buffer,
                                tx_buff_e[i].heca_dma.size,
                                tx_buff_e[i].heca_dma.dir);
                if (unlikely(!tx_buff_e[i].heca_dma.addr)) {
                        heca_printk(KERN_ERR "unable to create ib mapping");
                        ret = -EFAULT;
                        goto err;
                }

                tx_buff_e[i].wrk_req = kzalloc(sizeof(struct heca_msg_work_request),
                                GFP_KERNEL);
                if (!tx_buff_e[i].wrk_req) {
                        heca_printk(KERN_ERR "Failed to allocate wrk_req");
                        ret = -ENOMEM;
                        goto err;
                }

                tx_buff_e[i].wrk_req->wr_ele = kzalloc(sizeof(struct heca_work_request_element),
                                GFP_KERNEL);
                if (!tx_buff_e[i].wrk_req->wr_ele) {
                        heca_printk(KERN_ERR "Failed to allocate wrk_req->wr_ele");
                        ret = -ENOMEM;
                        goto err;
                }
                tx_buff_e[i].wrk_req->wr_ele->heca_dma = tx_buff_e[i].heca_dma;

                tx_buff_e[i].reply_work_req = kzalloc(sizeof(struct heca_reply_work_request),
                                GFP_KERNEL);
                if (!tx_buff_e[i].reply_work_req) {
                        heca_printk(KERN_ERR "Failed to allocate reply_work_req");
                        ret = -ENOMEM;
                        goto err;
                }

                tx_buff_e[i].reply_work_req->hwr_ele = kzalloc(
                                sizeof(struct heca_work_request_element),
                                GFP_KERNEL);
                if (!tx_buff_e[i].reply_work_req->hwr_ele) {
                        heca_printk(KERN_ERR "Failed to allocate reply_work_req->wr_ele");
                        ret = -ENOMEM;
                        goto err;
                }
                init_tx_ele(&tx_buff_e[i], conn, i);
        }
        goto done;

err:
        BUG_ON(!tx_buff_e);
        destroy_tx_buffer(conn);
        kfree(tx_buff_e);
done: return ret;
}

static void init_rx_ele(struct rx_buffer_element *rx_ele,
                struct heca_connection *conn)
{
        struct heca_recv_work_req_element *rwr = rx_ele->recv_wrk_rq_ele;
        struct ib_sge *recv_sge = &rwr->recv_sgl;

        recv_sge->addr = rx_ele->heca_dma.addr;
        recv_sge->length = rx_ele->heca_dma.size;
        recv_sge->lkey = conn->mr->lkey;

        rwr->sq_wr.next = NULL;
        rwr->sq_wr.num_sge = 1;
        rwr->sq_wr.sg_list = &rwr->recv_sgl;
        rwr->sq_wr.wr_id = rx_ele->id;
}

static int create_rx_buffer(struct heca_connection *conn)
{
        int i;
        int undo = 0;
        struct rx_buffer_element *rx;

        conn->rx_buffer.len = get_nb_rx_buff_elements(conn);
        rx = kzalloc((sizeof(struct rx_buffer_element) * conn->rx_buffer.len),
                        GFP_KERNEL);
        if (!rx)
                goto err_buf;
        conn->rx_buffer.rx_buf = rx;

        for (i = 0; i < conn->rx_buffer.len; ++i) {
                rx[i].hmsg_buffer = kzalloc(sizeof(struct heca_message),
                                GFP_KERNEL);
                if (!rx[i].hmsg_buffer)
                        goto err1;

                rx[i].heca_dma.size = sizeof(struct heca_message);
                rx[i].heca_dma.dir = DMA_BIDIRECTIONAL;
                rx[i].heca_dma.addr = ib_dma_map_single(conn->cm_id->device,
                                rx[i].hmsg_buffer,
                                rx[i].heca_dma.size,
                                rx[i].heca_dma.dir);
                if (!rx[i].heca_dma.addr)
                        goto err2;

                rx[i].recv_wrk_rq_ele = kzalloc(sizeof(struct heca_recv_work_req_element),
                                GFP_KERNEL);
                if (!rx[i].recv_wrk_rq_ele)
                        goto err3;

                rx[i].id = i;
                init_rx_ele(&rx[i], conn);
        }

        return 0;

err3:
        ib_dma_unmap_single(conn->cm_id->device, rx[i].heca_dma.addr,
                        rx[i].heca_dma.size, rx[i].heca_dma.dir);
err2:
        kfree(rx[i].hmsg_buffer);
err1:
        for (undo = 0; undo < i; ++undo) {
                ib_dma_unmap_single(conn->cm_id->device, rx[undo].heca_dma.addr,
                                rx[undo].heca_dma.size, rx[undo].heca_dma.dir);
                kfree(rx[undo].hmsg_buffer);
                kfree(rx[undo].recv_wrk_rq_ele);
        }
        kfree(rx);
        conn->rx_buffer.rx_buf = 0;
err_buf:
        heca_printk(KERN_ERR "RX BUFFER NOT CREATED");
        return -1;
}

static void format_rdma_info(struct heca_connection *conn)
{
        conn->rid.send_buf->node_ip = htonl(conn->hcm->node_ip);
        conn->rid.send_buf->buf_rx_addr = htonll((u64) conn->rx_buffer.rx_buf);
        conn->rid.send_buf->buf_msg_addr = htonll((u64) conn->tx_buffer.tx_buf);
        conn->rid.send_buf->rx_buf_size = htonl(conn->rx_buffer.len);
        conn->rid.send_buf->rkey_msg = htonl(conn->mr->rkey);
        conn->rid.send_buf->rkey_rx = htonl(conn->mr->rkey);
        conn->rid.send_buf->flag = RDMA_INFO_CL;
}

static int create_rdma_info(struct heca_connection *conn)
{
        int size = sizeof(struct heca_rdma_info);
        struct rdma_info_data *rid = &conn->rid;

        rid->send_buf = kzalloc(size, GFP_KERNEL);
        if (unlikely(!rid->send_buf))
                goto send_mem_err;

        rid->send_dma.size = size;
        rid->send_dma.dir = DMA_TO_DEVICE;
        rid->send_dma.addr = ib_dma_map_single(conn->cm_id->device,
                        rid->send_buf,
                        rid->send_dma.size,
                        rid->send_dma.dir);
        if (unlikely(!rid->send_dma.addr))
                goto send_info_err;

        rid->recv_buf = kzalloc(size, GFP_KERNEL);
        if (unlikely(!rid->recv_buf))
                goto recv_mem_err;

        rid->recv_dma.size = size;
        rid->recv_dma.dir = DMA_FROM_DEVICE;
        rid->recv_dma.addr = ib_dma_map_single(conn->cm_id->device,
                        rid->recv_buf,
                        rid->recv_dma.size,
                        rid->recv_dma.dir);
        if (unlikely(!rid->send_dma.addr))
                goto recv_info_err;

        rid->remote_info = kzalloc(size, GFP_KERNEL);
        if (unlikely(!rid->remote_info))
                goto remote_info_buffer_err;

        rid->remote_info->flag = RDMA_INFO_CL;
        rid->exchanged = 2;
        format_rdma_info(conn);
        return 0;

remote_info_buffer_err:
        heca_printk(KERN_ERR "ERROR : NO REMOTE INFO BUFFER");
        ib_dma_unmap_single(conn->cm_id->device, rid->recv_dma.addr,
                        rid->recv_dma.size, rid->recv_dma.dir);

recv_info_err:
        heca_printk(KERN_ERR "ERROR : NO RECV INFO BUFFER");
        kfree(rid->recv_buf);

recv_mem_err:
        heca_printk(KERN_ERR "no memory allocated for the reception buffer");
        ib_dma_unmap_single(conn->cm_id->device, rid->send_dma.addr,
                        rid->send_dma.size, rid->send_dma.dir);

send_info_err:
        heca_printk(KERN_ERR "ERROR : NO SEND INFO BUFFER");
        kfree(rid->send_buf);

send_mem_err:
        heca_printk("no memory allocated for the sending buffer");
        return -1;
}

static int init_tx_lists(struct heca_connection *conn)
{
        int i;
        struct tx_buffer *tx = &conn->tx_buffer;
        int max_tx_send = conn->tx_buffer.len / 3;

        tx->request_queue_sz = 0;
        init_llist_head(&tx->request_queue);
        init_llist_head(&tx->tx_free_elements_list);
        init_llist_head(&tx->tx_free_elements_list_reply);
        spin_lock_init(&tx->tx_free_elements_list_lock);
        spin_lock_init(&tx->tx_free_elements_list_reply_lock);
        INIT_LIST_HEAD(&tx->ordered_request_queue);
        mutex_init(&tx->flush_mutex);
        INIT_WORK(&conn->delayed_request_flush_work,
                        delayed_request_flush_work_fn);

        for (i = 0; i < max_tx_send; ++i)
                release_tx_element(conn, &tx->tx_buf[i]);

        for (; i < conn->tx_buffer.len; ++i)
                release_tx_element_reply(conn, &tx->tx_buf[i]);

        return 0;
}

static int setup_connection(struct heca_connection *conn, int type)
{
        int ret = 0, err = 0;
        struct rdma_conn_param conn_param;

        conn->pd = ib_alloc_pd(conn->cm_id->device);
        if (!conn->pd)
                goto err1;
        conn->mr = ib_get_dma_mr(conn->pd,
                        IB_ACCESS_LOCAL_WRITE |
                        IB_ACCESS_REMOTE_READ |
                        IB_ACCESS_REMOTE_WRITE);
        if (!conn->mr)
                goto err2;
        if (setup_qp_attr(conn))
                goto err3;
        if (setup_qp(conn))
                goto err4;
        if (create_tx_buffer(conn))
                goto err5;
        if (create_rx_buffer(conn))
                goto err6;
        if (heca_init_page_pool(conn))
                goto err7;
        if (create_rdma_info(conn))
                goto err8;
        if (init_tx_lists(conn))
                goto err9;

        if (type) {
                heca_recv_info(conn);

                memset(&conn_param, 0, sizeof(struct rdma_conn_param));
                conn_param.responder_resources = 1;
                conn_param.initiator_depth = 1;

                if (rdma_accept(conn->cm_id, &conn_param))
                        goto err10;
        }

        return ret;

err10: err++;
err9: err++;
err8: err++;
err7: err++;
err6: err++;
err5: err++;
err4: err++;
err3: err++;
err2: err++;
err1: err++;
      heca_printk(KERN_ERR "Could not setup connection: error %d", err);
      return err;
}

int client_event_handler(struct rdma_cm_id *id, struct rdma_cm_event *ev)
{
        int ret = 0, err = 0;
        struct heca_connection *conn = id->context;

        switch (ev->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
                ret = rdma_resolve_route(id, 2000);
                if (ret)
                        goto err1;
                break;

        case RDMA_CM_EVENT_ROUTE_RESOLVED:
                ret = setup_connection(conn, 0);
                if (ret)
                        goto err2;

                ret = connect_client(id);
                if (ret) {
                        complete(&conn->completion);
                        goto err3;
                }

                atomic_set(&conn->alive, 1);
                break;

        case RDMA_CM_EVENT_ESTABLISHED:
                ret = heca_recv_info(conn);
                if (ret)
                        goto err4;

                ret = heca_send_info(conn);
                if (ret < 0)
                        goto err5;

                break;

        case RDMA_CM_EVENT_DISCONNECTED:
                if (likely(atomic_cmpxchg(&conn->alive, 1, -1) == 1))
                        schedule_destroy_conns();
                break;

        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
                heca_printk(KERN_ERR, "could not connect, %d", ev->event);
                complete(&conn->completion);
                break;

        case RDMA_CM_EVENT_DEVICE_REMOVAL:
        case RDMA_CM_EVENT_ADDR_CHANGE:
                heca_printk(KERN_ERR "unexpected event: %d", ev->event);
                ret = rdma_disconnect(id);
                if (unlikely(ret))
                        goto disconnect_err;
                break;

        default:
                heca_printk(KERN_ERR "no special handling: %d", ev->event);
                break;
        }

        return ret;

err5:
        err++;
err4:
        err++;
err3:
        err++;
err2:
        err++;
err1:
        err++;
        ret = rdma_disconnect(id);
        heca_printk(KERN_ERR, "fatal error %d", err);
        if (unlikely(ret))
                goto disconnect_err;

        return ret;

disconnect_err:
        heca_printk(KERN_ERR, "disconection failed");
        return ret;
}

int server_event_handler(struct rdma_cm_id *id, struct rdma_cm_event *ev)
{
        int ret = 0;
        struct heca_connection *conn = 0;
        struct heca_connections_manager *hcm;

        switch (ev->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
                break;

        case RDMA_CM_EVENT_CONNECT_REQUEST:
                conn = vzalloc(sizeof(struct heca_connection));
                if (!conn)
                        goto out;

                init_completion(&conn->completion);
                hcm = id->context;
                conn->hcm = hcm;
                conn->cm_id = id;
                id->context = conn;

                ret = setup_connection(conn, 1);
                if (ret) {
                        heca_printk(KERN_ERR "setup_connection failed: %d",
                                        ret);
                        goto err;
                }

                ret = create_connection_sysfs_entry(conn);
                if (ret) {
                        heca_printk(KERN_ERR "create_conn_sysfs_entry failed: %d",
                                        ret);
                        goto err;
                }

                atomic_set(&conn->alive, 1);
                break;

        case RDMA_CM_EVENT_ESTABLISHED:
                break;

        case RDMA_CM_EVENT_DISCONNECTED:
                conn = id->context;
                if (likely(atomic_cmpxchg(&conn->alive, 1, -1) == 1))
                        schedule_destroy_conns();
                break;

        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_DEVICE_REMOVAL:
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
        case RDMA_CM_EVENT_ADDR_CHANGE:
                heca_printk(KERN_ERR "unexpected event: %d", ev->event);

                ret = rdma_disconnect(id);
                if (unlikely(ret))
                        goto disconnect_err;
                break;

        default:
                heca_printk(KERN_ERR "no special handling: %d", ev->event);
                break;
        }

out:
        return ret;

disconnect_err:
        heca_printk(KERN_ERR "disconnect failed");
err:
        vfree(conn);
        conn = 0;
        return ret;
}

static void free_rdma_info(struct heca_connection *conn)
{
        if (conn->rid.send_dma.addr) {
                ib_dma_unmap_single(conn->cm_id->device,
                                conn->rid.send_dma.addr,
                                conn->rid.send_dma.size,
                                conn->rid.send_dma.dir);
                kfree(conn->rid.send_buf);
        }

        if (conn->rid.recv_dma.addr) {
                ib_dma_unmap_single(conn->cm_id->device,
                                conn->rid.recv_dma.addr,
                                conn->rid.recv_dma.size,
                                conn->rid.recv_dma.dir);
                kfree(conn->rid.recv_buf);
        }

        if (conn->rid.remote_info) {
                kfree(conn->rid.remote_info);
        }

        memset(&conn->rid, 0, sizeof(struct rdma_info_data));
}

void release_tx_element(struct heca_connection *conn,
                struct tx_buffer_element *tx_e)
{
        struct tx_buffer *tx = &conn->tx_buffer;
        atomic_set(&tx_e->used, 0);
        atomic_set(&tx_e->released, 0);
        llist_add(&tx_e->tx_buf_ele_ptr, &tx->tx_free_elements_list);
}

void release_tx_element_reply(struct heca_connection *conn,
                struct tx_buffer_element *tx_e)
{
        struct tx_buffer *tx = &conn->tx_buffer;
        atomic_set(&tx_e->used, 0);
        atomic_set(&tx_e->released, 0);
        llist_add(&tx_e->tx_buf_ele_ptr, &tx->tx_free_elements_list_reply);
}

void try_release_tx_element(struct heca_connection *conn,
                struct tx_buffer_element *tx_e)
{
        if (atomic_add_return(1, &tx_e->released) == 2)
                release_tx_element(conn, tx_e);
}

int create_connection(struct heca_connections_manager *hcm,
                unsigned long ip,
                unsigned short port)
{
        struct rdma_conn_param param;
        struct heca_connection *conn;

        conn = vzalloc(sizeof(struct heca_connection));
        if (unlikely(!conn))
                goto err;

        memset(&param, 0, sizeof(struct rdma_conn_param));
        param.responder_resources = 1;
        param.initiator_depth = 1;
        param.retry_count = 10;

        conn->local.sin_family = AF_INET;
        conn->local.sin_addr.s_addr = hcm->sin.sin_addr.s_addr;
        conn->local.sin_port = 0;

        conn->remote.sin_family = AF_INET;
        conn->remote.sin_addr.s_addr = ip;
        conn->remote.sin_port = port;

        init_completion(&conn->completion);
        conn->remote_node_ip = ip;
        insert_rb_conn(conn);

        conn->hcm = hcm;
        conn->cm_id = rdma_create_id(client_event_handler, conn, RDMA_PS_TCP,
                        IB_QPT_RC);
        if (IS_ERR(conn->cm_id))
                goto err1;

        if (create_connection_sysfs_entry(conn)) {
                heca_printk(KERN_ERR "create_conn_sysfs_entry failed");
                goto err1;
        }

        return rdma_resolve_addr(conn->cm_id, (struct sockaddr *) &conn->local,
                        (struct sockaddr*) &conn->remote, 2000);

err1: erase_rb_conn(conn);
      vfree(conn);
err: return -1;
}

static void remove_hprocs_for_conn(struct heca_connection *conn)
{
        struct heca_space *hspace;
        struct heca_process *hproc;
        struct list_head *pos, *n, *it;

        list_for_each (pos, &get_heca_module_state()->hspaces_list) {
                hspace = list_entry(pos, struct heca_space, hspace_ptr);
                list_for_each_safe (it, n, &hspace->hprocs_list) {
                        hproc = list_entry(it, struct heca_process,
                                        hproc_ptr);
                        if (hproc->connection == conn)
                                remove_hproc(hspace->hspace_id,
                                                hproc->hproc_id);
                }
        }
}

int destroy_connection(struct heca_connection *conn)
{
        int ret = 0;

        remove_hprocs_for_conn(conn);

        if (likely(conn->cm_id)) {
                synchronize_rcu();
                cancel_work_sync(&conn->recv_work);
                cancel_work_sync(&conn->send_work);

                if (likely(conn->cm_id->qp))
                        ret |= ib_destroy_qp(conn->cm_id->qp);

                if (likely(conn->qp_attr.send_cq))
                        ret |= ib_destroy_cq(conn->qp_attr.send_cq);

                if (likely(conn->qp_attr.recv_cq))
                        ret |= ib_destroy_cq(conn->qp_attr.recv_cq);

                if (likely(conn->mr))
                        ret |= ib_dereg_mr(conn->mr);

                if (likely(conn->pd))
                        ret |= ib_dealloc_pd(conn->pd);

                destroy_rx_buffer(conn);
                destroy_tx_buffer(conn);
                free_rdma_info(conn);
                rdma_destroy_id(conn->cm_id);
        }

        heca_destroy_page_pool(conn);

        erase_rb_conn(conn);
        delete_connection_sysfs_entry(conn);
        vfree(conn);

        return ret;
}

