/*
 * Christopher Hunter <christopher.hunter@sap.com> 2013 (c)
 */

#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <rdma/rdma_cm.h>
#include <rdma/ib_verbs.h>

#include "rdma_transport.h"
#include "hutils.h"

#include "conn.h"
#include "ops.h"
#include "ioctl.h"
#include "pull.h"
#include "transport.h"

int destroy_rdma(struct rdma_transport *rt)
{
        int rc = 0;

        if (!rt)
		goto done;

        if (rt->cm_id->qp) {
                ib_destroy_qp(rt->cm_id->qp);
                rt->cm_id->qp = NULL;
        }

        if (rt->listen_cq) {
                ib_destroy_cq(rt->listen_cq);
                rt->listen_cq = NULL;
        }

        if (rt->mr) {
                ib_dereg_mr(rt->mr);
                rt->mr = NULL;
        }

        if (rt->pd) {
                ib_dealloc_pd(rt->pd);
                rt->pd = NULL;
        }

        rdma_destroy_id(rt->cm_id);
        rt->cm_id = NULL;

done:
	heca_printk(KERN_DEBUG "<exit> %d", rc);
	return rc;
}

int create_rdma(struct heca_module_state *heca_state, struct
                heca_transport_manager *htm, struct transport *rtransport,
                unsigned short port)
{
        int ret;
        struct rdma_transport *rt;

	rtransport->type = RDMA;
	rtransport->context = (void*)kmalloc(sizeof(struct rdma_transport),
                        GFP_KERNEL);

        rt = (struct rdma_transport*)rtransport->context;

        rt->cm_id = rdma_create_id(server_event_handler, htm, RDMA_PS_TCP,
                        IB_QPT_RC);

        if (IS_ERR(rt->cm_id)) {
                rt->cm_id = NULL;
                ret = PTR_ERR(rt->cm_id);
                heca_printk(KERN_ERR "Failed rdma_create_id: %d", ret);
                goto failed;
        }

        rt->sin.sin_family = AF_INET;
        rt->sin.sin_addr.s_addr = htm->node_ip;
        rt->sin.sin_port = port;

        ret = rdma_bind_addr(rt->cm_id, (struct sockaddr *)&rt->sin);
        if (ret) {
                heca_printk(KERN_ERR "Failed rdma_bind_addr: %d", ret);
                goto failed;
        }

        rt->pd = ib_alloc_pd(rt->cm_id->device);
        if (IS_ERR(rt->pd)) {
                ret = PTR_ERR(rt->pd);
                rt->pd = NULL;
                heca_printk(KERN_ERR "Failed id_alloc_pd: %d", ret);
                goto failed;
        }

        rt->listen_cq = ib_create_cq(rt->cm_id->device, listener_cq_handle,
                        NULL, rt, 2, 0);
        if (IS_ERR(rt->listen_cq)) {
                ret = PTR_ERR(rt->listen_cq);
                rt->listen_cq = NULL;
                heca_printk(KERN_ERR "Failed ib_create_cq: %d", ret);
                goto failed;
        }

        if ((ret = ib_req_notify_cq(rt->listen_cq, IB_CQ_NEXT_COMP))) {
                heca_printk(KERN_ERR "Failed ib_req_notify_cq: %d", ret);
                goto failed;
        }

        rt->mr = ib_get_dma_mr(rt->pd, IB_ACCESS_LOCAL_WRITE |
                        IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE);
        if (IS_ERR(rt->mr)) {
                ret = PTR_ERR(rt->mr);
                rt->mr = NULL;
                heca_printk(KERN_ERR "Failed ib_get_dma_mr: %d", ret);
                goto failed;
        }

        heca_state->htm = htm;

        ret = rdma_listen(rt->cm_id, 2);
        if (ret){
                heca_state->htm = NULL;
                heca_printk(KERN_ERR "Failed rdma_listen: %d", ret);
                goto failed;
        }

        return ret;
failed:
        kfree(htm);
        return ret;
}
