#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <netinet/ip.h>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <ev.h>
#include "util.h"
#include "nfq.h"
#include "conf.h"
#include "filter.h"

void mainLoop();
void initFilter(const std::string &file);

DEFINE_string(c, "", "path to the configuration file");
DECLARE_int32(v);

int main(int argc, char **argv) {
    google::InitGoogleLogging(argv[0]);
    google::ParseCommandLineFlags(&argc, &argv, true);
    std::string configFile = FLAGS_c;

    if (getuid() != 0) {
        LOG(ERROR) << "This program must be run as root.";
        return -1;
    }

    if (configFile.empty()) {
        LOG(ERROR) << "no config file!";
        return -1;
    }

    LOG(INFO) << "get config filename: " << configFile;
    initFilter(configFile);

    mainLoop();
    return 0;
}

void initFilter(const std::string &file) {
    std::vector<ConfItem> ci;
    auto filter = PacketFilter::getInstance();
    CHECK(parseConfigFile(file, ci)) << "syntax error in config file";
    for (auto &c : ci) {
        LOG(INFO) << "got config: " << std::hex << c.ip() << " " << c.method() << std::endl;
        CHECK(filter->add(c)) << "add filter failed";
    }
}

// mock or unmock the ip.protocol
// for system would check the TCP packet before enter the nfhook
// when inv == false, change the proto to 50 + proto
// otherwise          recover the proto
void fixProtocol(uint8_t &proto, bool inv) {
    if (inv) {
        switch (proto) {
        case 6:
            proto = 0 + 50;
            break;
        case 17:
            proto = 1 + 50;
            break;
        default:
            break;
        }
    } else {
        switch (proto - 50) {
        case 0:
            proto = 6;
            break;
        case 1:
            proto = 17;
            break;
        default:
            break;
        }
    }
}

static int queue_cb(std::shared_ptr<NFQ_queue> qh, struct nfgenmsg *nfmsg, struct nfq_data *nfa) {
    static auto filter = PacketFilter::getInstance();
    const size_t extraLen = MY_TAG_SIZE + MBEDTLS_MD_MAX_SIZE;

	u_int32_t id;
    struct nfqnl_msg_packet_hdr *ph;
    uint8_t *data;
    int datalen;
    struct pkt_buff *pkt;
    struct iphdr *ip;
    char srcIpBuf[INET6_ADDRSTRLEN];
    char dstIpBuf[INET6_ADDRSTRLEN];

	ph = nfq_get_msg_packet_hdr(nfa);
	id = ntohl(ph->packet_id);
    datalen = nfq_get_payload(nfa, &data);
    uint8_t lastByte = data[datalen - 1];
    pkt = pktb_alloc(AF_INET, data, datalen, extraLen);
    PCHECK(pkt != nullptr) << "pktb_alloc failed";
    ip = nfq_ip_get_hdr(pkt);
    uint16_t iphdrLen = 4 * ip->ihl;

    inet_ntop(AF_INET, &ip->saddr, srcIpBuf, sizeof srcIpBuf);
    inet_ntop(AF_INET, &ip->daddr, dstIpBuf, sizeof dstIpBuf);
    VLOG(2) << std::hex << srcIpBuf << " -> " << dstIpBuf;
    
    uint32_t verdict = NF_ACCEPT;
    PacketFilter::key_type key;
    key.d.proto = 0;

    bool crypt = false;
    bool pass = true;
    uint8_t ipProto = ip->protocol;
    PacketFilter::transer_type transer;
    switch(ph->hook) {
    case NF_IP_LOCAL_OUT:
        key.d.ip = ip->daddr;
        VLOG(2) << "got an outgoing packet, key: " << std::hex << key.key;
        transer = filter->find(key);
        if (!transer) pass = true;
        else if (filter->match(key, ipProto)) {
            VLOG(2) << "proto matched: " << (uint32_t)ipProto;
            pass = false;
            crypt = true;
            ip->protocol = FAKE_PROTO;
        }
        break;

    case NF_IP_PRE_ROUTING:
        key.d.ip = ip->saddr;
        VLOG(2) << "got an incoming packet, key: " << std::hex << key.key;
        transer = filter->find(key);
        if (!transer) pass = true;
        else if (ipProto == FAKE_PROTO && filter->match(key, lastByte)) {
            VLOG(2) << "proto matched: " << (uint32_t)lastByte;
            pass = false;
            crypt = false;
            ip->protocol = lastByte;
        }
        break;

    default:
        LOG(WARNING) << "unexpected hook: " << ph->hook;
        pass = true;
        break;
    }

    ssize_t deltLen = 0;
    ssize_t newBodyLen = ip->tot_len - iphdrLen;
    if (pass) {
        VLOG(1) << "not match, skip this packet";
        verdict = NF_ACCEPT;
    } else {
        VLOG(1) << "transforming the packet, crypt: " << crypt;
        ip->tot_len = ntohs(ip->tot_len);
        verdict = (transer->accept() ? NF_ACCEPT : NF_DROP); 
        ssize_t bodyLen = ip->tot_len - iphdrLen;
        newBodyLen = transer->transform(crypt,
                           pktb_network_header(pkt) + iphdrLen,
                           bodyLen, bodyLen + extraLen,
                           &ipProto);
        if (newBodyLen > 0) {
            deltLen = newBodyLen - bodyLen;
            ip->tot_len += deltLen;
            ip->tot_len = htons(ip->tot_len);
            nfq_ip_set_checksum(ip);
            VLOG(1) << "old len: " << bodyLen << ", new len: " << newBodyLen;
        } else {
            ip->tot_len = htons(ip->tot_len);
            LOG(WARNING) << "transer failed, will drop this packet";
            verdict = NF_DROP;
        }
    }

    int result;
    if (pass || newBodyLen <= 0) {
        result = qh->set_verdict(id, verdict, 0, nullptr);
    } else {
        int newPktLen = static_cast<int>(pktb_len(pkt)) + deltLen;
        result = qh->set_verdict(id, verdict, newPktLen, pktb_data(pkt));
        VLOG(2) << "verdict: " << verdict <<  " packet old len: " << pktb_len(pkt)
                << ", new len: " << newPktLen << ", delta: " << deltLen
                << ", result: " << result;
    }
    pktb_free(pkt);

    return result;
}

static void pkt_arrived_cb(EV_P_ ev_io *wc, int revents) {
    uint8_t buf[65536];
    ssize_t len;
    queue_io *w = reinterpret_cast<queue_io *>(wc);

    len = recv(w->io.fd, buf, sizeof buf, 0);
    PCHECK(len >= 0) << "recv failed!";
    if (len == 0) {
        LOG(WARNING) << "fake wake up!";
        return;
    }

    VLOG(1) << len << " bytes received, start to handle packet";
    w->handle->handle_packet(buf, len);
}

static void interrupt_cb(EV_P_ ev_signal *w, int revents) {
    LOG(WARNING) << "SIGNAL received";
    ev_signal_stop(EV_A_ w);
    ev_break(EV_A);
}


void mainLoop() {
    struct ev_loop *loop = ev_loop_new();
    queue_io queue_watcher;
    struct ev_signal int_watcher;
    struct ev_signal term_watcher;

    ev_signal_init(&int_watcher, interrupt_cb, SIGINT);
    ev_signal_start(EV_A_ &int_watcher);
    ev_signal_init(&term_watcher, interrupt_cb, SIGTERM);
    ev_signal_start(EV_A_ &term_watcher);

    LOG(INFO) << "Opening handle.";
    auto h = std::make_shared<NFQ>();
	if (!h) {
        LOG(ERROR) << "error during nfq_open";
		exit(1);
	}

	LOG(INFO) << "unbinding existing nf_queue handler for AF_INET (if any)";
    PCHECK(nfq_unbind_pf(h->native_handle(), AF_INET) >= 0) << "error during nfq_unbind_pf()";

	LOG(INFO) << "binding nfnetlink_queue as nf_queue handler for AF_INET";
	PCHECK(nfq_bind_pf(h->native_handle(), AF_INET) >= 0) << "error during nfq_bind_pf()";

	LOG(INFO) << "binding this socket to queue '0'";
    auto qh = h->create_queue(queue_cb);

	LOG(INFO) << "setting copy_packet mode";
	if (qh->set_mode(NFQNL_COPY_PACKET, 0xffff) < 0) {
		LOG(ERROR) << "can't set packet_copy mode";
		exit(1);
	}

    ev_io_init(&queue_watcher.io, pkt_arrived_cb, h->get_fd(), EV_READ);
    queue_watcher.handle = h;
    set_nonblocking(queue_watcher.io.fd);
    ev_io_start(EV_A_ &queue_watcher.io);

    ev_run(EV_A_ 0);

    ev_loop_destroy(EV_A);
}

