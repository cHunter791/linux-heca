/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 */
#include <linux/list.h>

#include "ioctl.h"
#include "trace.h"
#include "conn.h"
#include "base.h"
#include "struct.h"
#include "ops.h"
#include "sysfs.h"
#include "transport_manager.h"
#include "rdma.h"

#define ntohll(x) be64_to_cpu(x)
#define htonll(x) cpu_to_be64(x)

static struct kmem_cache *kmem_heca_request_cache;

unsigned long inet_addr(const char *cp)
{
        unsigned int a, b, c, d;
        unsigned char arr[4];

        sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d);
        arr[0] = a;
        arr[1] = b;
        arr[2] = c;
        arr[3] = d;
        return *(unsigned int*) arr; /* network */
}

char *inet_ntoa(unsigned long s_addr, char *buf, int sz)
{
        unsigned char *b = (unsigned char *)&s_addr;

        if (!sz)
                return NULL;

        buf[0] = 0;
        snprintf(buf, sz - 1, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return buf;
}

char *port_ntoa(unsigned short port, char *buf, int sz)
{
        if (!sz)
                return NULL;

        buf[0] = 0;
        snprintf(buf, sz - 1, "%u", ntohs(port));
        return buf;
}

char *sockaddr_ntoa(struct sockaddr_in *sa, char *buf, int sz)
{
        char ip_str[20], port_str[10];

        if (!sz)
                return NULL;

        buf[0] = 0;
        inet_ntoa(sa->sin_addr.s_addr, ip_str, sizeof ip_str);
        port_ntoa(sa->sin_port, port_str, sizeof port_str);
        snprintf(buf, sz - 1, "%s:%s", ip_str, port_str);
        return buf;
}

char *conn_ntoa(struct sockaddr_in *local, struct sockaddr_in *remote,
                char *buf, int sz)
{
        char local_str[35], remote_str[35];

        if (!sz)
                return NULL;

        sockaddr_ntoa(local, local_str, sizeof local_str);
        sockaddr_ntoa(remote, remote_str, sizeof remote_str);
        buf[0] = 0;
        snprintf(buf, sz - 1, "%s-%s", local_str, remote_str);
        return buf;
}

static inline void init_kmem_request_cache_elm(void *obj)
{
        struct heca_request *req = (struct heca_request *) obj;
        memset(req, 0, sizeof(struct heca_request));
}

void init_kmem_heca_request_cache(void)
{
        kmem_heca_request_cache = kmem_cache_create("heca_request",
                        sizeof(struct heca_request), 0,
                        SLAB_HWCACHE_ALIGN | SLAB_TEMPORARY,
                        init_kmem_request_cache_elm);
}

void destroy_kmem_heca_request_cache(void)
{
        kmem_cache_destroy(kmem_heca_request_cache);
}

inline struct heca_request *alloc_heca_request(void)
{
        return kmem_cache_alloc(kmem_heca_request_cache, GFP_KERNEL);
}

inline void release_heca_request(struct heca_request *req)
{
        kmem_cache_free(kmem_heca_request_cache, req);
}

static inline int get_nb_tx_buff_elements(struct heca_connection *conn)
{
        return conn->qp_attr.cap.max_send_wr >> 1;
}

static inline int get_nb_rx_buff_elements(struct heca_connection *conn)
{
        return conn->qp_attr.cap.max_recv_wr;
}

static int get_max_pushed_reqs(struct heca_connection *conn)
{
        return get_nb_tx_buff_elements(conn) << 2;
}

static void schedule_delayed_request_flush(struct heca_connection *conn)
{
        schedule_work(&conn->delayed_request_flush_work);
}

static inline void queue_heca_request(struct heca_connection *conn,
                struct heca_request *req)
{
        trace_heca_queued_request(req->hspace_id, req->local_hproc_id,
                        req->remote_hproc_id, req->hmr_id, 0, req->addr,
                        req->type, -1);
        llist_add(&req->lnode, &conn->tx_buffer.request_queue);
        schedule_delayed_request_flush(conn);
}

int add_heca_request(struct heca_request *req, struct heca_connection *conn,
                u16 type, u32 hspace_id, u32 src_id, u32 mr_id, u32 dest_id,
                unsigned long addr, int (*func)(struct tx_buffer_element *),
                struct heca_page_cache *hpc, struct page *page,
                struct heca_page_pool_element *ppe, int need_ppe,
                struct heca_message *msg)
{
        if (!req) {
                req = kmem_cache_alloc(kmem_heca_request_cache, GFP_KERNEL);
                if (unlikely(!req))
                        return -ENOMEM;
        }

        req->type = type;
        req->hspace_id = hspace_id;
        req->hmr_id = mr_id;
        req->local_hproc_id = src_id;
        req->remote_hproc_id = dest_id;
        req->addr = addr;
        req->func = func;
        req->hpc = hpc;
        req->page = page;
        req->ppe = ppe;
        req->need_ppe = need_ppe;

        if (msg) {
                heca_msg_cpy(&req->hmsg, msg);
                req->response = 1;
        } else {
                req->response = 0;
        }

        queue_heca_request(conn, req);

        return 0;
}

inline int heca_request_queue_empty(struct heca_connection *conn)
{
        /* we are not 100% accurate but it's ok we can have a few sneaking in */
        return (llist_empty(&conn->tx_buffer.request_queue) &&
                        list_empty(&conn->tx_buffer.ordered_request_queue));
}

inline int heca_request_queue_full(struct heca_connection *conn)
{
        return conn->tx_buffer.request_queue_sz > get_max_pushed_reqs(conn);
}

/* this will copy the offset and rkey of the original and send them back! */
static inline void heca_tx_response_prepare(struct tx_buffer_element *tx_e,
                struct heca_message *msg)
{
        heca_msg_cpy(tx_e->hmsg_buffer, msg);
        tx_e->wrk_req->dst_addr = NULL;
}

static void heca_tx_prepare(struct heca_connection *conn,
                struct tx_buffer_element *tx_e, u32 hspace_id, u32 mr_id,
                u32 src_id, u32 dest_id, unsigned long shared_addr,
                struct heca_page_cache *hpc, struct page *page,
                struct heca_page_pool_element *ppe, int need_ppe)
{
        struct heca_message *msg = tx_e->hmsg_buffer;

        while (need_ppe && !ppe) {
                might_sleep();
                ppe = heca_prepare_ppe(conn, page);
                if (likely(ppe))
                        break;
                cond_resched();
        }

        msg->offset = tx_e->id;
        msg->hspace_id = hspace_id;
        msg->src_id = src_id;
        msg->dest_id = dest_id;
        msg->mr_id = mr_id;
        msg->req_addr = (u64) shared_addr;
        msg->dst_addr = (u64) (need_ppe? ppe->page_buf : 0);
        msg->rkey = conn->mr->rkey; /* TODO: is this needed? */

        tx_e->wrk_req->dst_addr = ppe;
        tx_e->wrk_req->hpc = hpc;
        tx_e->reply_work_req->mem_page = page;
}

int heca_send_tx_e(struct heca_connection *conn, struct tx_buffer_element *tx_e,
                int resp, int type, u32 hspace_id, u32 mr_id,
                u32 src_id, u32 dest_id, unsigned long local_addr,
                unsigned long shared_addr, struct heca_page_cache *hpc,
                struct page *page, struct heca_page_pool_element *ppe,
                int need_ppe, int (*func)(struct tx_buffer_element *),
                struct heca_message *msg)
{
        if (resp) {
                heca_tx_response_prepare(tx_e, msg);
        } else {
                heca_tx_prepare(conn, tx_e, hspace_id, mr_id, src_id, dest_id,
                                shared_addr, hpc, page, ppe, need_ppe);
        }

        tx_e->hmsg_buffer->type = type;
        tx_e->callback.func = func;

        trace_heca_send_request(hspace_id, src_id, dest_id, mr_id, local_addr,
                        shared_addr, type);

        return tx_heca_send(conn, tx_e);
}

void heca_request_queue_merge(struct tx_buffer *tx)
{
        struct list_head *head = &tx->ordered_request_queue;
        struct llist_node *llnode = llist_del_all(&tx->request_queue);

        while (llnode) {
                struct heca_request *req;

                req = container_of(llnode, struct heca_request, lnode);
                list_add_tail(&req->ordered_list, head);
                head = &req->ordered_list;
                llnode = llnode->next;
                tx->request_queue_sz++;
        }
}

static inline int flush_heca_request_queue(struct heca_connection *conn)
{
        struct tx_buffer *tx = &conn->tx_buffer;
        struct heca_request *req;
        struct tx_buffer_element *tx_e = NULL;
        int ret = 0;

        mutex_lock(&tx->flush_mutex);
        heca_request_queue_merge(tx);
        while (!list_empty(&tx->ordered_request_queue)) {
                tx_e = try_get_next_empty_tx_ele(conn, 0);
                if (!tx_e) {
                        ret = 1;
                        break;
                }
                tx->request_queue_sz--;
                req = list_first_entry(&tx->ordered_request_queue,
                                struct heca_request, ordered_list);
                trace_heca_flushing_requests(tx->request_queue_sz);
                heca_send_tx_e(conn, tx_e, req->response, req->type,
                                req->hspace_id, req->hmr_id,
                                req->local_hproc_id, req->remote_hproc_id, 0,
                                req->addr, req->hpc, req->page, req->ppe,
                                req->need_ppe, req->func, &req->hmsg);
                list_del(&req->ordered_list);
                release_heca_request(req);
        }
        mutex_unlock(&tx->flush_mutex);
        return ret;

}

static void delayed_request_flush_work_fn(struct work_struct *w)
{
        struct heca_connection *conn;
        udelay(REQUEST_FLUSH_DELAY);
        conn = container_of(w, struct heca_connection,
                        delayed_request_flush_work);
        if (flush_heca_request_queue(conn))
                schedule_delayed_request_flush(conn);
}

static void destroy_connection_work(struct work_struct *work)
{
        struct heca_connections_manager *hcm = get_heca_module_state()->hcm;
        struct rb_root *root;
        struct rb_node *node, *next;
        struct heca_connection *conn;
        unsigned long seq;

        do {
                seq = read_seqbegin(&hcm->connections_lock);
                root = &hcm->connections_rb_tree_root;
                for (node = rb_first(root); node; node = next) {
                        conn = rb_entry(node, struct heca_connection, rb_node);
                        next = rb_next(node);
                        if (atomic_cmpxchg(&conn->alive, -1, 0) == -1)
                                destroy_connection(conn);
                }
        } while (read_seqretry(&hcm->connections_lock, seq));

        kfree(work);
}

static inline void schedule_destroy_conns(void)
{
        struct work_struct *work = kmalloc(sizeof(struct work_struct),
                        GFP_KERNEL);
        INIT_WORK(work, destroy_connection_work);
        schedule_work(work);
}

static inline void queue_recv_work(struct heca_connection *conn)
{
        rcu_read_lock();
        if (atomic_read(&conn->alive))
                queue_work(get_heca_module_state()->heca_rx_wq,
                                &conn->recv_work);
        rcu_read_unlock();
}

static inline void queue_send_work(struct heca_connection *conn)
{
        rcu_read_lock();
        if (atomic_read(&conn->alive))
                queue_work(get_heca_module_state()->heca_tx_wq,
                                &conn->send_work);
        rcu_read_unlock();
}

static inline void handle_tx_element(struct heca_connection *conn,
                struct tx_buffer_element *tx_e,
                int (*callback)(struct heca_connection *,
                        struct tx_buffer_element *))
{
        /* if tx_e->used > 2, we're racing with release_heca_tx_elements */
        if (atomic_add_return(1, &tx_e->used) == 2) {
                if (callback)
                        callback(conn, tx_e);
                try_release_tx_element(conn, tx_e);
        }
}

static int refill_recv_wr(struct heca_connection *conn,
                struct rx_buffer_element *rx_e)
{
        int ret = 0;
        ret = ib_post_recv(conn->cm_id->qp, &rx_e->recv_wrk_rq_ele->sq_wr,
                        &rx_e->recv_wrk_rq_ele->bad_wr);
        if (ret)
                heca_printk(KERN_ERR "Failed ib_post_recv(offset=%d): %d",
                                rx_e->id, ret);

        return ret;
}

static int heca_recv_message_handler(struct heca_connection *conn,
                struct rx_buffer_element *rx_e)
{
        struct tx_buffer_element *tx_e = NULL;

        trace_heca_rx_msg(rx_e->hmsg_buffer->hspace_id, rx_e->hmsg_buffer->src_id,
                        rx_e->hmsg_buffer->dest_id, rx_e->hmsg_buffer->mr_id, 0,
                        rx_e->hmsg_buffer->req_addr, rx_e->hmsg_buffer->type,
                        rx_e->hmsg_buffer->offset);

        switch (rx_e->hmsg_buffer->type) {
        case MSG_RES_PAGE:
                BUG_ON(rx_e->hmsg_buffer->offset < 0 ||
                                rx_e->hmsg_buffer->offset >=
                                conn->tx_buffer.len);
                tx_e = &conn->tx_buffer.tx_buf[rx_e->hmsg_buffer->offset];
                handle_tx_element(conn, tx_e, process_page_response);
                break;
        case MSG_RES_PAGE_REDIRECT:
                tx_e = &conn->tx_buffer.tx_buf[rx_e->hmsg_buffer->offset];
                process_page_redirect(conn, tx_e, rx_e->hmsg_buffer->dest_id);
                break;
        case MSG_RES_PAGE_FAIL:
                BUG_ON(rx_e->hmsg_buffer->offset < 0 ||
                                rx_e->hmsg_buffer->offset >=
                                conn->tx_buffer.len);
                tx_e = &conn->tx_buffer.tx_buf[rx_e->hmsg_buffer->offset];
                tx_e->hmsg_buffer->type = MSG_RES_PAGE_FAIL;
                handle_tx_element(conn, tx_e, process_page_response);
                break;
        case MSG_REQ_PUSHED_PAGE:
        case MSG_REQ_PAGE:
        case MSG_REQ_READ:
                process_page_request_msg(conn, rx_e->hmsg_buffer);
                break;
        case MSG_REQ_CLAIM:
        case MSG_REQ_CLAIM_TRY:
                process_page_claim(conn, rx_e->hmsg_buffer);
                break;
        case MSG_REQ_PUSH:
                process_pull_request(conn, rx_e);
                ack_msg(conn, rx_e->hmsg_buffer, MSG_RES_ACK);
                break;
        case MSG_RES_HPROC_FAIL:
                process_hproc_status(conn, rx_e);
                break;
        case MSG_RES_ACK:
        case MSG_RES_ACK_FAIL:
                BUG_ON(rx_e->hmsg_buffer->offset < 0 ||
                                rx_e->hmsg_buffer->offset >=
                                conn->tx_buffer.len);
                tx_e = &conn->tx_buffer.tx_buf[rx_e->hmsg_buffer->offset];
                if (tx_e->hmsg_buffer->type &
                                (MSG_REQ_CLAIM | MSG_REQ_CLAIM_TRY))
                        process_claim_ack(conn, tx_e, rx_e->hmsg_buffer);
                handle_tx_element(conn, tx_e, NULL);
                break;
        case MSG_REQ_QUERY:
                process_request_query(conn, rx_e);
                break;
        case MSG_RES_QUERY:
                BUG_ON(rx_e->hmsg_buffer->offset < 0 ||
                                rx_e->hmsg_buffer->offset >=
                                conn->tx_buffer.len);
                tx_e = &conn->tx_buffer.tx_buf[rx_e->hmsg_buffer->offset];
                process_query_info(tx_e);
                handle_tx_element(conn, tx_e, NULL);
                break;
        default:
                heca_printk(KERN_ERR "unhandled message stats addr: %p, status %d id %d",
                                rx_e, rx_e->hmsg_buffer->type, rx_e->id);
                goto err;
        }

        refill_recv_wr(conn, rx_e);
        return 0;
err:
        return 1;
}

static int heca_send_message_handler(struct heca_connection *conn,
                struct tx_buffer_element *tx_e)
{
        trace_heca_tx_msg(tx_e->hmsg_buffer->hspace_id, tx_e->hmsg_buffer->src_id,
                        tx_e->hmsg_buffer->dest_id, -1, 0,
                        tx_e->hmsg_buffer->req_addr, tx_e->hmsg_buffer->type,
                        tx_e->hmsg_buffer->offset);

        switch (tx_e->hmsg_buffer->type) {
        case MSG_RES_PAGE:
                if (!pte_present(tx_e->reply_work_req->pte)) {
                        heca_clear_swp_entry_flag(tx_e->reply_work_req->mm,
                                        tx_e->reply_work_req->addr,
                                        tx_e->reply_work_req->pte,
                                        HECA_INFLIGHT_BITPOS);
                }
                heca_ppe_clear_release(conn, &tx_e->wrk_req->dst_addr);
                release_tx_element_reply(conn, tx_e);
                break;

                /* we can immediately discard the tx_e */
        case MSG_RES_ACK:
        case MSG_RES_ACK_FAIL:
        case MSG_RES_PAGE_FAIL:
        case MSG_RES_HPROC_FAIL:
        case MSG_RES_QUERY:
        case MSG_RES_PAGE_REDIRECT:
                release_tx_element(conn, tx_e);
                break;

                /* we keep the tx_e alive, to process the response */
        case MSG_REQ_PAGE:
        case MSG_REQ_READ:
        case MSG_REQ_PUSHED_PAGE:
        case MSG_REQ_PUSH:
        case MSG_REQ_CLAIM:
        case MSG_REQ_CLAIM_TRY:
        case MSG_REQ_QUERY:
                try_release_tx_element(conn, tx_e);
                break;
        default:
                heca_printk(KERN_ERR "unhandled message stats  addr: %p, status %d , id %d",
                                tx_e, tx_e->hmsg_buffer->type, tx_e->id);
                return 1;
        }
        return 0;
}

static void heca_cq_event_handler(struct ib_event *event, void *data)
{
        heca_printk(KERN_DEBUG "event %u  data %p", event->event, data);
}

void listener_cq_handle(struct ib_cq *cq, void *cq_context)
{
        struct ib_wc wc;
        int ret = 0;

        if (ib_req_notify_cq(cq, IB_CQ_SOLICITED))
                heca_printk(KERN_INFO "Failed ib_req_notify_cq");

        if ((ret = ib_poll_cq(cq, 1, &wc)) > 0) {
                if (likely(wc.status == IB_WC_SUCCESS)) {
                        switch (wc.opcode) {
                        case IB_WC_RECV:
                                break;
                        default: {
                                heca_printk(KERN_ERR "expected opcode %d got %d",
                                                IB_WC_SEND, wc.opcode);
                                break;
                        }
                        }
                } else
                        heca_printk(KERN_ERR "Unexpected type of wc");
        } else if (unlikely(ret < 0)) {
                heca_printk(KERN_ERR "recv FAILUREi %d", ret);
        }
}

static void heca_send_poll(struct ib_cq *cq)
{
        struct ib_wc wc;
        struct heca_connection *ele = (struct heca_connection *) cq->cq_context;

        while (ib_poll_cq(cq, 1, &wc) > 0) {
                if (unlikely(wc.status != IB_WC_SUCCESS ||
                                        wc.opcode != IB_WC_SEND))
                        continue;

                if (unlikely(ele->rid.exchanged))
                        ele->rid.exchanged--;
                else
                        heca_send_message_handler(ele,
                                        &ele->tx_buffer.tx_buf[wc.wr_id]);
        }
}

static void reg_rem_info(struct heca_connection *conn)
{
        conn->rid.remote_info->node_ip = ntohl(conn->rid.recv_buf->node_ip);
        conn->rid.remote_info->buf_rx_addr = ntohll(conn->rid.recv_buf->buf_rx_addr);
        conn->rid.remote_info->buf_msg_addr = ntohll(conn->rid.recv_buf->buf_msg_addr);
        conn->rid.remote_info->rx_buf_size = ntohl(conn->rid.recv_buf->rx_buf_size);
        conn->rid.remote_info->rkey_msg = ntohl(conn->rid.recv_buf->rkey_msg);
        conn->rid.remote_info->rkey_rx = ntohl(conn->rid.recv_buf->rkey_rx);
        conn->rid.remote_info->flag = conn->rid.recv_buf->flag;
}

static int heca_send_info(struct heca_connection *conn)
{
        struct rdma_info_data *rid = &conn->rid;

        rid->send_sge.addr = rid->send_dma.addr;
        rid->send_sge.length = rid->send_dma.size;
        rid->send_sge.lkey = conn->mr->lkey;

        rid->send_wr.next = NULL;
        rid->send_wr.wr_id = 0;
        rid->send_wr.sg_list = &rid->send_sge;
        rid->send_wr.num_sge = 1;
        rid->send_wr.opcode = IB_WR_SEND;
        rid->send_wr.send_flags = IB_SEND_SIGNALED;
        heca_printk(KERN_DEBUG "sending info");
        return ib_post_send(conn->cm_id->qp, &rid->send_wr, &rid->send_bad_wr);
}

static int heca_recv_info(struct heca_connection *conn)
{
        struct rdma_info_data *rid = &conn->rid;

        rid->recv_sge.addr = rid->recv_dma.addr;
        rid->recv_sge.length = rid->recv_dma.size;
        rid->recv_sge.lkey = conn->mr->lkey;

        rid->recv_wr.next = NULL;
        rid->recv_wr.wr_id = 0; // HECA2: unique id - address of data_struct
        rid->recv_wr.num_sge = 1;
        rid->recv_wr.sg_list = &rid->recv_sge;

        return ib_post_recv(conn->cm_id->qp, &rid->recv_wr, &rid->recv_bad_wr);
}

static int setup_recv_wr(struct heca_connection *conn)
{
        int i;
        struct rx_buffer_element *rx = conn->rx_buffer.rx_buf;

        if (unlikely(!rx))
                return -1;

        /* last rx elm reserved for initial info exchange */
        for (i = 0; i < conn->rx_buffer.len - 1; ++i) {
                if (refill_recv_wr(conn, &rx[i]))
                        return -1;
        }
        return 0;
}

static int exchange_info(struct heca_connection *conn, int id)
{
        int flag = (int) conn->rid.remote_info->flag;
        int ret = 0;
        struct heca_connection * conn_found;

        BUG_ON(!conn);

        if (unlikely(!conn->rid.recv_buf))
                goto err;
        flag = (int) conn->rid.remote_info->flag;

        switch (flag) {
        case RDMA_INFO_CL: {
                conn->rid.send_buf->flag = RDMA_INFO_SV;
                goto recv_send;
        }
        case RDMA_INFO_SV: {
                ret = heca_recv_info(conn);
                if (ret) {
                        heca_printk("could not post the receive work request");
                        goto err;
                }
                conn->rid.send_buf->flag = RDMA_INFO_READY_CL;
                ret = setup_recv_wr(conn);
                goto send;
        }
        case RDMA_INFO_READY_CL: {
                conn->rid.send_buf->flag = RDMA_INFO_READY_SV;
                ret = setup_recv_wr(conn);
                refill_recv_wr(conn,
                                &conn->rx_buffer.rx_buf[conn->rx_buffer.len - 1]);
                conn->rid.remote_info->flag = RDMA_INFO_NULL;

                conn->remote_node_ip = (u32) conn->rid.remote_info->node_ip;
                conn->remote.sin_addr.s_addr = (u32) conn->rid.remote_info->node_ip;
                conn->local = get_heca_module_state()->hcm->sin;
                conn_found = search_rb_conn(conn->remote_node_ip);

                if (conn_found) {
                        if (conn->remote_node_ip !=
                                        get_heca_module_state()->hcm->node_ip) {
                                char curr[20], prev[20];

                                inet_ntoa(conn->remote_node_ip,
                                                curr, sizeof curr);
                                inet_ntoa(conn_found->remote_node_ip,
                                                prev, sizeof prev);
                                heca_printk("destroy_connection duplicate: %s former: %s",
                                                curr, prev);
                                rdma_disconnect(conn->cm_id);
                        } else {
                                heca_printk("loopback, lets hope for the best");
                        }
                        erase_rb_conn(conn);
                } else {
                        char curr[20];

                        complete(&conn->completion);
                        insert_rb_conn(conn);
                        inet_ntoa(conn->remote_node_ip, curr, sizeof curr);
                        heca_printk("inserted conn_element to rb_tree: %s",
                                        curr);
                }
                goto send;

        }
        case RDMA_INFO_READY_SV: {
                refill_recv_wr(conn, &conn->rx_buffer.rx_buf[conn->rx_buffer.len - 1]);
                conn->rid.remote_info->flag = RDMA_INFO_NULL;
                //Server acknowledged --> connection is complete.
                //start sending messages.
                complete(&conn->completion);
                goto out;
        }
        default: {
                heca_printk(KERN_ERR "unknown RDMA info flag");
                goto out;
        }
        }

recv_send:
        ret = heca_recv_info(conn);
        if (ret < 0) {
                heca_printk(KERN_ERR "could not post the receive work request");
                goto err;
        }

send:
        ret = heca_send_info(conn);
        if (ret < 0) {
                heca_printk(KERN_ERR "could not post the send work request");
                goto err;
        }

out:
        return ret;

err:
        heca_printk(KERN_ERR "no receive info");
        return ret;
}

static void conn_recv_poll(struct ib_cq *cq)
{
        struct ib_wc wc;
        struct heca_connection *conn = (struct heca_connection *) cq->cq_context;

        while (ib_poll_cq(cq, 1, &wc) == 1) {
                if (likely(wc.status == IB_WC_SUCCESS)) {
                        if (unlikely(wc.opcode != IB_WC_RECV)) {
                                heca_printk(KERN_INFO "expected opcode %d got %d",
                                                IB_WC_RECV, wc.opcode);
                                continue;
                        }
                } else {
                        if (wc.status == IB_WC_WR_FLUSH_ERR) {
                                heca_printk(KERN_INFO "rx id %llx status %d vendor_err %x",
                                                wc.wr_id, wc.status,
                                                wc.vendor_err);
                        } else {
                                heca_printk(KERN_ERR "rx id %llx status %d vendor_err %x",
                                                wc.wr_id, wc.status,
                                                wc.vendor_err);
                        }
                        continue;
                }

                if (conn->rid.remote_info->flag) {
                        BUG_ON(wc.byte_len != sizeof(struct heca_rdma_info));
                        reg_rem_info(conn);
                        exchange_info(conn, wc.wr_id);
                } else {
                        BUG_ON(wc.byte_len != sizeof(struct heca_message));
                        BUG_ON(wc.wr_id < 0 || wc.wr_id >= conn->rx_buffer.len);
                        heca_recv_message_handler(conn,
                                        &conn->rx_buffer.rx_buf[wc.wr_id]);
                }
        }
}

static void send_cq_handle_work(struct work_struct *work)
{
        struct heca_connection *conn = container_of(work,
                        struct heca_connection, send_work);
        int ret = 0;

        heca_send_poll(conn->qp_attr.send_cq);
        ret = ib_req_notify_cq(conn->qp_attr.send_cq,
                        IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS);
        heca_send_poll(conn->qp_attr.send_cq);
        if (ret > 0)
                queue_send_work(conn);
}

static void recv_cq_handle_work(struct work_struct *work)
{
        struct heca_connection *conn = container_of(work,
                        struct heca_connection, recv_work);
        int ret = 0;

        conn_recv_poll(conn->qp_attr.recv_cq);
        ret = ib_req_notify_cq(conn->qp_attr.recv_cq,
                        IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS);
        conn_recv_poll(conn->qp_attr.recv_cq);
        if (ret > 0)
                queue_recv_work(conn);
}

static void send_cq_handle(struct ib_cq *cq, void *cq_context)
{
        queue_send_work((struct heca_connection *) cq->cq_context);
}

static void recv_cq_handle(struct ib_cq *cq, void *cq_context)
{
        queue_recv_work((struct heca_connection *) cq->cq_context);
}

inline void heca_msg_cpy(struct heca_message *dst, struct heca_message *orig)
{
        dst->hspace_id = orig->hspace_id;
        dst->src_id = orig->src_id;
        dst->dest_id = orig->dest_id;
        dst->type = orig->type;
        dst->offset = orig->offset;
        dst->req_addr = orig->req_addr;
        dst->dst_addr = orig->dst_addr;
        dst->rkey = orig->rkey;
}


int connect_hproc(__u32 hspace_id, __u32 hproc_id, unsigned long ip_addr,
                unsigned short port)
{
        int r = 0;
        struct heca_space *hspace;
        struct heca_process *hproc;
        struct heca_connection *conn;
        struct heca_module_state *heca_state = get_heca_module_state();

        hspace = find_hspace(hspace_id);
        if (!hspace) {
                heca_printk(KERN_ERR "can't find hspace %d", hspace_id);
                return -EFAULT;
        }

        heca_printk(KERN_ERR "connecting to hspace_id: %u [0x%p], hproc_id: %u",
                        hspace_id, hspace, hproc_id);

        mutex_lock(&hspace->hspace_mutex);
        hproc = find_hproc(hspace, hproc_id);
        if (!hproc) {
                heca_printk(KERN_ERR "can't find hproc %d", hproc_id);
                goto no_hproc;
        }

        conn = search_rb_conn(ip_addr);
        if (conn) {
                heca_printk(KERN_ERR "has existing connection to %pI4",
                                &ip_addr);
                goto done;
        }

        r = create_connection(heca_state->hcm, ip_addr, port);
        if (r) {
                heca_printk(KERN_ERR "create_connection failed %d", r);
                goto failed;
        }

        might_sleep();
        conn = search_rb_conn(ip_addr);
        if (!conn) {
                heca_printk(KERN_ERR "conneciton does not exist", r);
                r = -ENOLINK;
                goto failed;
        }

        wait_for_completion(&conn->completion);
        if (!atomic_read(&conn->alive)) {
                heca_printk(KERN_ERR "conneciton is not alive ... aborting");
                r = -ENOLINK;
                goto failed;
        }

done:
        hproc->connection = conn;

failed:
        release_hproc(hproc);
no_hproc:
        mutex_unlock(&hspace->hspace_mutex);
        heca_printk(KERN_INFO "hspace %d hproc %d hproc_connect ip %pI4: %d",
                        hspace_id, hproc_id, &ip_addr, r);
        return r;
}

struct tx_buffer_element *try_get_next_empty_tx_ele(
                struct heca_connection *conn,
                int require_empty_list)
{
        struct tx_buffer_element *tx_e = NULL;
        struct llist_node *llnode = NULL;

        spin_lock(&conn->tx_buffer.tx_free_elements_list_lock);
        if (!require_empty_list || heca_request_queue_empty(conn))
                llnode = llist_del_first(&conn->tx_buffer.tx_free_elements_list);
        spin_unlock(&conn->tx_buffer.tx_free_elements_list_lock);

        if (llnode) {
                tx_e = container_of(llnode, struct tx_buffer_element,
                                tx_buf_ele_ptr);
                atomic_set(&tx_e->used, 1);
        }
        return tx_e;
}

struct tx_buffer_element *try_get_next_empty_tx_reply_ele(
                struct heca_connection *conn)
{
        struct tx_buffer_element *tx_e = NULL;
        struct llist_node *llnode;

        spin_lock(&conn->tx_buffer.tx_free_elements_list_reply_lock);
        llnode = llist_del_first(&conn->tx_buffer.tx_free_elements_list_reply);
        spin_unlock(&conn->tx_buffer.tx_free_elements_list_reply_lock);

        if (llnode) {
                tx_e = container_of(llnode, struct tx_buffer_element,
                                tx_buf_ele_ptr);
                atomic_set(&tx_e->used, 1);
        }
        return tx_e;
}

/*
 * Can either fail with:
 *  > -ENOMEM - in which case we sleep and let ib work thread finish.
 *  > -ENOTCONN - meaning the connection has been disrupted; we handle this
 *                in destroy_connection.
 *  > -EINVAL (or other) - we sent wrong output, shouldn't happen.
 *
 */
int tx_heca_send(struct heca_connection *conn, struct tx_buffer_element *tx_e)
{
        int ret;
        int type = tx_e->hmsg_buffer->type;

retry:
        switch (type) {
        case MSG_REQ_PAGE:
        case MSG_REQ_PUSHED_PAGE:
        case MSG_REQ_READ:
        case MSG_REQ_CLAIM:
        case MSG_REQ_CLAIM_TRY:
        case MSG_REQ_QUERY:
        case MSG_REQ_PUSH:
        case MSG_RES_PAGE_REDIRECT:
        case MSG_RES_PAGE_FAIL:
        case MSG_RES_HPROC_FAIL:
        case MSG_RES_ACK:
        case MSG_RES_ACK_FAIL:
        case MSG_RES_QUERY:
                ret = ib_post_send(conn->cm_id->qp, &tx_e->wrk_req->wr_ele->wr,
                                &tx_e->wrk_req->wr_ele->bad_wr);
                break;
        case MSG_RES_PAGE:
                ret = ib_post_send(conn->cm_id->qp, &tx_e->reply_work_req->wr,
                                &tx_e->reply_work_req->hwr_ele->bad_wr);
                break;
        default:
                BUG();
        }

        /*
         * we have no other choice but to postpone and try again (no memory for a
         * queued request). this should happen mainly with softiwarp.
         */
        if (unlikely(ret == -ENOMEM)) {
                cond_resched();
                goto retry;
        }

        if (ret && ret != -ENOTCONN) {
                heca_printk("ib_post_send() returned %d on type 0x%x",
                                ret, type);
                BUG();
        }
        return ret;
}

