#ifndef HECA_RDMA_H_
#define HECA_RDMA_H_

#include "transport_manager.h"

#define IB_MAX_CAP_SCQ      256
#define IB_MAX_CAP_RCQ      1024    /* Heuristic; perhaps raise in the future */
#define IB_MAX_SEND_SGE     2
#define IB_MAX_RECV_SGE     2

#define IW_MAX_CAP_SCQ      256
#define IW_MAX_CAP_RCQ      1024    /* Heuristic; perhaps raise in the future */
#define IW_MAX_SEND_SGE     2
#define IW_MAX_RECV_SGE     2

#define RDMA_INFO_CL        4
#define RDMA_INFO_SV        3
#define RDMA_INFO_READY_CL  2
#define RDMA_INFO_READY_SV  1
#define RDMA_INFO_NULL      0

struct map_dma {
        dma_addr_t addr;
        u64 size;
        u64 dir;
};

struct rdma_info_data {
        struct heca_rdma_info *send_buf;
        struct heca_rdma_info *recv_buf;

        struct map_dma send_dma;
        struct map_dma recv_dma;
        struct heca_rdma_info *remote_info;

        struct ib_sge recv_sge;
        struct ib_recv_wr recv_wr;
        struct ib_recv_wr *recv_bad_wr;

        struct ib_sge send_sge;
        struct ib_send_wr send_wr;
        struct ib_send_wr *send_bad_wr;
        int exchanged;
};

struct heca_rdma_info {

        u8 flag;
        u32 node_ip;
        u64 buf_msg_addr;
        u32 rkey_msg;
        u64 buf_rx_addr;
        u32 rkey_rx;
        u32 rx_buf_size;
};

struct rx_buffer {
        struct rx_buffer_element *rx_buf;
        int len;
};

struct tx_buffer {
        struct tx_buffer_element *tx_buf;
        int len;

        struct llist_head tx_free_elements_list;
        struct llist_head tx_free_elements_list_reply;
        spinlock_t tx_free_elements_list_lock;
        spinlock_t tx_free_elements_list_reply_lock;

        struct llist_head request_queue;
        struct mutex  flush_mutex;
        struct list_head ordered_request_queue;
        int request_queue_sz;
        struct work_struct delayed_request_flush_work;
};

struct heca_work_request_element {
        struct heca_connection *connection;
        struct ib_send_wr wr;
        struct ib_sge sg;
        struct ib_send_wr *bad_wr;
        struct map_dma heca_dma;
};

struct heca_msg_work_request {
        struct heca_work_request_element *wr_ele;
        struct heca_page_pool_element *dst_addr;
        struct heca_page_cache *hpc;
};

struct heca_recv_work_req_element {
        struct heca_connection *connection;
        struct ib_recv_wr sq_wr;
        struct ib_recv_wr *bad_wr;
        struct ib_sge recv_sgl;
};

struct heca_reply_work_request {
        //The one for sending back a message
        struct heca_work_request_element *hwr_ele;

        //The one for sending the page
        struct ib_send_wr wr;
        struct ib_send_wr *bad_wr;
        struct page * mem_page;
        void *page_buf;
        struct ib_sge page_sgl;
        pte_t pte;
        struct mm_struct *mm;
        unsigned long addr;
};

int client_event_handler(struct rdma_cm_id *, struct rdma_cm_event *);
int server_event_handler(struct rdma_cm_id *, struct rdma_cm_event *);
int create_connection(struct heca_connections_manager *, unsigned long, unsigned short);
int destroy_connection(struct heca_connection *);
void release_tx_element(struct heca_connection *, struct tx_buffer_element *);
void release_tx_element_reply(struct heca_connection *, struct tx_buffer_element *);
void try_release_tx_element(struct heca_connection *, struct tx_buffer_element *);

#endif /* HECA_RDMA_H_ */
