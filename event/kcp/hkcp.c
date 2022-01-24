#include "hkcp.h"

#if WITH_KCP

#include "hevent.h"
#include "hlog.h"

static kcp_setting_t s_kcp_setting;

static int __kcp_output(const char* buf, int len, ikcpcb* ikcp, void* userdata) {
    // printf("ikcp_output len=%d\n", len);
    rudp_entry_t* rudp = (rudp_entry_t*)userdata;
    assert(rudp != NULL && rudp->io != NULL);
    int nsend = sendto(rudp->io->fd, buf, len, 0, &rudp->addr.sa, SOCKADDR_LEN(&rudp->addr));
    // printf("sendto nsend=%d\n", nsend);
    return nsend;
}

static void __kcp_update_timer_cb(htimer_t* timer) {
    rudp_entry_t* rudp = (rudp_entry_t*)timer->privdata;
    assert(rudp != NULL && rudp->io != NULL && rudp->kcp.ikcp != NULL);
    ikcp_update(rudp->kcp.ikcp, (IUINT32)(rudp->io->loop->cur_hrtime / 1000));
}

void kcp_release(kcp_t* kcp) {
    if (kcp->ikcp == NULL) return;
    if (kcp->update_timer) {
        htimer_del(kcp->update_timer);
        kcp->update_timer = NULL;
    }
    HV_FREE(kcp->readbuf.base);
    kcp->readbuf.len = 0;
    // printf("ikcp_release ikcp=%p\n", kcp->ikcp);
    ikcp_release(kcp->ikcp);
    kcp->ikcp = NULL;
}

int hio_set_kcp(hio_t* io, kcp_setting_t* setting) {
    io->io_type = HIO_TYPE_KCP;
    io->kcp_setting = setting;
    return 0;
}

kcp_t* hio_get_kcp(hio_t* io, uint32_t conv) {
    rudp_entry_t* rudp = hio_get_rudp(io);
    assert(rudp != NULL);
    kcp_t* kcp = &rudp->kcp;
    if (kcp->ikcp != NULL) return kcp;
    if (io->kcp_setting == NULL) {
        io->kcp_setting = &s_kcp_setting;
    }
    kcp_setting_t* setting = io->kcp_setting;
    kcp->ikcp = ikcp_create(conv, rudp);
    // printf("ikcp_create conv=%u ikcp=%p\n", conv, kcp->ikcp);
    kcp->ikcp->output = __kcp_output;
    kcp->conv = conv;
    if (setting->interval > 0) {
        ikcp_nodelay(kcp->ikcp, setting->nodelay, setting->interval, setting->fastresend, setting->nocwnd);
    }
    if (setting->sndwnd > 0 && setting->rcvwnd > 0) {
        ikcp_wndsize(kcp->ikcp, setting->sndwnd, setting->rcvwnd);
    }
    if (setting->mtu > 0) {
        ikcp_setmtu(kcp->ikcp, setting->mtu);
    }
    if (kcp->update_timer == NULL) {
        int update_interval = setting->update_interval;
        if (update_interval == 0) {
            update_interval = DEFAULT_KCP_UPDATE_INTERVAL;
        }
        kcp->update_timer = htimer_add(io->loop, __kcp_update_timer_cb, update_interval, INFINITE);
        kcp->update_timer->privdata = rudp;
    }
    // NOTE: alloc kcp->readbuf when hio_read_kcp
    return kcp;
}

int hio_write_kcp(hio_t* io, const void* buf, size_t len) {
    IUINT32 conv = io->kcp_setting ? io->kcp_setting->conv : 0;
    kcp_t* kcp = hio_get_kcp(io, conv);
    int nsend = ikcp_send(kcp->ikcp, (const char*)buf, len);
    // printf("ikcp_send len=%d nsend=%d\n", (int)len, nsend);
    if (nsend < 0) {
        hloge("ikcp_send error: %d", nsend);
        return nsend;
    }
    ikcp_update(kcp->ikcp, (IUINT32)io->loop->cur_hrtime / 1000);
    return len;
}

int hio_read_kcp (hio_t* io, void* buf, int readbytes) {
    IUINT32 conv = ikcp_getconv(buf);
    kcp_t* kcp = hio_get_kcp(io, conv);
    if (kcp->conv != conv) {
        hloge("recv invalid kcp packet!");
        hio_close_rudp(io, io->peeraddr);
        return -1;
    }
    // printf("ikcp_input len=%d\n", readbytes);
    ikcp_input(kcp->ikcp, (const char*)buf, readbytes);
    if (kcp->readbuf.base == NULL || kcp->readbuf.len == 0) {
        kcp->readbuf.len = DEFAULT_KCP_READ_BUFSIZE;
        HV_ALLOC(kcp->readbuf.base, kcp->readbuf.len);
    }
    int ret = 0;
    while (1) {
        int nrecv = ikcp_recv(kcp->ikcp, kcp->readbuf.base, kcp->readbuf.len);
        // printf("ikcp_recv nrecv=%d\n", nrecv);
        if (nrecv < 0) break;
        hio_read_cb(io, kcp->readbuf.base, nrecv);
        ret += nrecv;
    }
    return ret;
}

#endif
