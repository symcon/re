/**
 * @file sip/request.c  SIP Request
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re_types.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_sa.h>
#include <re_list.h>
#include <re_fmt.h>
#include <re_dns.h>
#include <re_uri.h>
#include <re_sys.h>
#include <re_udp.h>
#include <re_msg.h>
#include <re_sip.h>
#include "sip.h"


struct sip_request {
	struct le le;
	struct list cachel;
	struct list addrl;
	struct list srvl;
	struct sip_request **reqp;
	struct sip_ctrans *ct;
	struct dns_query *dnsq;
	struct dns_query *dnsq2;
	struct sip *sip;
	char *met;
	char *uri;
	char *host;
	char *branch;
	struct mbuf *mb;
	sip_send_h *sendh;
	sip_resp_h *resph;
	void *arg;
	size_t sortkey;
	enum sip_transp tp;
	bool tp_selected;
	bool stateful;
	bool canceled;
	bool provrecv;
	uint16_t port;
};


static int  request_next(struct sip_request *req);
static bool rr_append_handler(struct dnsrr *rr, void *arg);
static void srv_handler(int err, const struct dnshdr *hdr, struct list *ansl,
			struct list *authl, struct list *addl, void *arg);
static int  srv_lookup(struct sip_request *req, const char *domain);
static int  addr_lookup(struct sip_request *req, const char *name);


static int str_ldup(char **dst, const char *src, int len)
{
	struct pl pl;

	pl.p = src;
	pl.l = len < 0 ? str_len(src) : (size_t)len;

	return pl_strdup(dst, &pl);
}


static void destructor(void *arg)
{
	struct sip_request *req = arg;

	if (req->reqp && req->stateful) {
		/* user does deref before request has completed */
		*req->reqp = NULL;
		req->reqp  = NULL;
		req->sendh = NULL;
		req->resph = NULL;
		sip_request_cancel(mem_ref(req));
		return;
	}

	list_flush(&req->cachel);
	list_flush(&req->addrl);
	list_flush(&req->srvl);
	list_unlink(&req->le);
	mem_deref(req->dnsq);
	mem_deref(req->dnsq2);
	mem_deref(req->ct);
	mem_deref(req->met);
	mem_deref(req->uri);
	mem_deref(req->host);
	mem_deref(req->branch);
	mem_deref(req->mb);
}


static void terminate(struct sip_request *req, int err,
		      const struct sip_msg *msg)
{
	if (req->reqp) {
		*req->reqp = NULL;
		req->reqp = NULL;
	}

	list_unlink(&req->le);
	req->sendh = NULL;

	if (req->resph) {
		req->resph(err, msg, req->arg);
		req->resph = NULL;
	}
}


static bool close_handler(struct le *le, void *arg)
{
	struct sip_request *req = le->data;
	(void)arg;

	req->dnsq  = mem_deref(req->dnsq);
	req->dnsq2 = mem_deref(req->dnsq2);
	req->ct    = mem_deref(req->ct);

	terminate(req, ECONNABORTED, NULL);
	mem_deref(req);

	return false;
}


static void response_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct sip_request *req = arg;

	if (msg && msg->scode < 200) {
		if (!req->provrecv) {
			req->provrecv = true;
			if (req->canceled)
				(void)sip_ctrans_cancel(req->ct);
		}

		if (req->resph)
			req->resph(err, msg, req->arg);

		return;
	}

	req->ct = NULL;

	if (!req->canceled && (err || (msg && msg->scode == 503)) &&
	    (req->addrl.head || req->srvl.head)) {

		err = request_next(req);
		if (!err)
			return;
	}

	terminate(req, err, msg);
	mem_deref(req);
}


static int connect_handler(struct sa *src, const struct sa *dst,
			   struct mbuf *mb, void *arg)
{
	struct sip_request *req = arg;
	struct mbuf *mbs  = NULL;
	struct mbuf *cont = NULL;
	int err;

	if (!sa_isset(src, SA_ALL))
		return EINVAL;

	mbuf_set_posend(mb, 0, 0);
	mbs = mbuf_alloc(256);
	if (!mbs)
		return ENOMEM;

	err = req->sendh ? req->sendh(req->tp, src, dst, mbs, &cont,
				      req->arg) : 0;
	if (err)
		goto out;

	mbuf_set_pos(mbs, 0);
	err  = mbuf_printf(mb, "%s %s SIP/2.0\r\n", req->met, req->uri);
	err |= mbuf_printf(mb, "Via: SIP/2.0/%s %J;branch=%s;rport\r\n",
			   sip_transp_name(req->tp), src, req->branch);
	err |= mbuf_write_mem(mb, mbuf_buf(mbs), mbuf_get_left(mbs));
	err |= mbuf_write_mem(mb, mbuf_buf(req->mb), mbuf_get_left(req->mb));
	if (cont) {
		err |= mbuf_write_mem(mb, mbuf_buf(cont), mbuf_get_left(cont));
		mem_deref(cont);
	}

	if (err)
		goto out;

	mb->pos = 0;

out:
	if (err)
		mbuf_reset(mb);

	mem_deref(mbs);
	return err;
}


static int request(struct sip_request *req, enum sip_transp tp,
		   const struct sa *dst)
{
	struct mbuf *mb   = NULL;
	int err = ENOMEM;
	struct sa laddr;

	req->provrecv = false;

	mem_deref(req->branch);
	(void)re_sdprintf(&req->branch, "z9hG4bK%016llx", rand_u64());
	mb  = mbuf_alloc(1024);
	if (!req->branch || !mb)
		goto out;


	if (!req->branch || !mb)
		goto out;

	err = sip_transp_laddr(req->sip, &laddr, tp, dst);
	if (err)
		goto out;

	if (!req->stateful) {
		err = sip_send_conn(req->sip, NULL, tp, dst, mb,
				    connect_handler, req);
	}
	else {
		err = sip_ctrans_request(&req->ct, req->sip, tp, dst, req->met,
					 req->branch, req->host, mb,
					 connect_handler, response_handler,
					 req);
	}
	if (err)
		goto out;

 out:
	mem_deref(mb);

	return err;
}


static int request_next(struct sip_request *req)
{
	struct dnsrr *rr;
	struct sa dst;
	int err;

 again:
	rr = list_ledata(req->addrl.head);
	if (!rr) {
		rr = list_ledata(req->srvl.head);
		if (!rr)
			return ENOENT;

		req->port = rr->rdata.srv.port;

		dns_rrlist_apply2(&req->cachel, rr->rdata.srv.target,
				  DNS_TYPE_A, DNS_TYPE_AAAA, DNS_CLASS_IN,
				  true, rr_append_handler, &req->addrl);

		list_unlink(&rr->le);

		if (req->addrl.head) {
			dns_rrlist_sort_addr(&req->addrl, req->sortkey);
			mem_deref(rr);
			goto again;
		}

		err = addr_lookup(req, rr->rdata.srv.target);
		mem_deref(rr);

		return err;
	}

	switch (rr->type) {

	case DNS_TYPE_A:
		sa_set_in(&dst, rr->rdata.a.addr, req->port);
		break;

	case DNS_TYPE_AAAA:
		sa_set_in6(&dst, rr->rdata.aaaa.addr, req->port);
		break;

	default:
		return EINVAL;
	}

	list_unlink(&rr->le);
	mem_deref(rr);

	err = request(req, req->tp, &dst);
	if (err) {
		if (req->addrl.head || req->srvl.head)
			goto again;
	}
	else if (!req->stateful) {
		req->resph = NULL;
		terminate(req, 0, NULL);
		mem_deref(req);
	}

	return err;
}


static bool transp_next(struct sip *sip, enum sip_transp *tp)
{
	enum sip_transp i;

	for (i=(enum sip_transp)(*tp+1); i<SIP_TRANSPC; i++) {

		if (!sip_transp_supported(sip, i, AF_UNSPEC))
			continue;

		*tp = i;
		return true;
	}

	return false;
}


static bool transp_next_srv(struct sip *sip, enum sip_transp *tp)
{
	enum sip_transp i;

	for (i=(enum sip_transp)(*tp-1); i>SIP_TRANSP_NONE; i--) {
		const char *srvid;

		srvid = sip_transp_srvid(i);
		if (0 == str_cmp(srvid, "???"))
			continue;

		if (!sip_transp_supported(sip, i, AF_UNSPEC))
			continue;

		*tp = i;
		return true;
	}

	return false;
}


static bool transp_first(struct sip *sip, enum sip_transp *tp)
{
	if (!sip || !tp)
		return false;

	if (sip->tp_def != SIP_TRANSP_NONE &&
			sip_transp_supported(sip, sip->tp_def, AF_UNSPEC)) {
		*tp = sip->tp_def;
		return true;
	}

	*tp = SIP_TRANSP_NONE;
	return transp_next(sip, tp);
}


static bool rr_append_handler(struct dnsrr *rr, void *arg)
{
	struct list *lst = arg;

	switch (rr->type) {

	case DNS_TYPE_A:
	case DNS_TYPE_AAAA:
	case DNS_TYPE_SRV:
		if (rr->le.list)
			break;

		list_append(lst, &rr->le, mem_ref(rr));
		break;
	}

	return false;
}


static bool rr_cache_handler(struct dnsrr *rr, void *arg)
{
	struct sip_request *req = arg;

	switch (rr->type) {

	case DNS_TYPE_A:
		if (!sip_transp_supported(req->sip, req->tp, AF_INET))
			break;

		list_unlink(&rr->le_priv);
		list_append(&req->cachel, &rr->le_priv, rr);
		break;

#ifdef HAVE_INET6
	case DNS_TYPE_AAAA:
		if (!sip_transp_supported(req->sip, req->tp, AF_INET6))
			break;

		list_unlink(&rr->le_priv);
		list_append(&req->cachel, &rr->le_priv, rr);
		break;
#endif

	case DNS_TYPE_CNAME:
		list_unlink(&rr->le_priv);
		list_append(&req->cachel, &rr->le_priv, rr);
		break;
	}

	return false;
}


static bool rr_naptr_handler(struct dnsrr *rr, void *arg)
{
	struct sip_request *req = arg;
	enum sip_transp tp;

	if (rr->type != DNS_TYPE_NAPTR)
		return false;

	if (!str_casecmp(rr->rdata.naptr.services, "SIP+D2U"))
		tp = SIP_TRANSP_UDP;
	else if (!str_casecmp(rr->rdata.naptr.services, "SIP+D2T"))
		tp = SIP_TRANSP_TCP;
	else if (!str_casecmp(rr->rdata.naptr.services, "SIPS+D2T"))
		tp = SIP_TRANSP_TLS;
	else if (!str_casecmp(rr->rdata.naptr.services, "SIP+D2W"))
		tp = SIP_TRANSP_WS;
	else if (!str_casecmp(rr->rdata.naptr.services, "SIPS+D2W"))
		tp = SIP_TRANSP_WSS;
	else
		return false;

	if (!sip_transp_supported(req->sip, tp, AF_UNSPEC))
		return false;

	req->tp = tp;
	req->tp_selected = true;

	return true;
}


static void naptr_handler(int err, const struct dnshdr *hdr, struct list *ansl,
			  struct list *authl, struct list *addl, void *arg)
{
	struct sip_request *req = arg;
	struct dnsrr *rr;
	(void)hdr;
	(void)authl;

	dns_rrlist_sort(ansl, DNS_TYPE_NAPTR, req->sortkey);

	rr = dns_rrlist_apply(ansl, NULL, DNS_TYPE_NAPTR, DNS_CLASS_IN, false,
			      rr_naptr_handler, req);
	if (!rr) {
		req->tp = SIP_TRANSPC;
		if (!transp_next_srv(req->sip, &req->tp)) {
			err = EPROTONOSUPPORT;
			goto fail;
		}

		err = srv_lookup(req, req->host);
		if (err)
			goto fail;

		return;
	}

	dns_rrlist_apply(addl, rr->rdata.naptr.replace, DNS_TYPE_SRV,
			 DNS_CLASS_IN, true, rr_append_handler, &req->srvl);

	if (!req->srvl.head) {
		err = dnsc_query(&req->dnsq, req->sip->dnsc,
				 rr->rdata.naptr.replace, DNS_TYPE_SRV,
				 DNS_CLASS_IN, true, srv_handler, req);
		if (err)
			goto fail;

		return;
	}

	dns_rrlist_sort(&req->srvl, DNS_TYPE_SRV, req->sortkey);

	dns_rrlist_apply(addl, NULL, DNS_QTYPE_ANY, DNS_CLASS_IN, false,
			 rr_cache_handler, req);

	err = request_next(req);
	if (err)
		goto fail;

	return;

 fail:
	terminate(req, err, NULL);
	mem_deref(req);
}


static void srv_handler(int err, const struct dnshdr *hdr, struct list *ansl,
			 struct list *authl, struct list *addl, void *arg)
{
	struct sip_request *req = arg;
	(void)hdr;
	(void)authl;

	dns_rrlist_apply(ansl, NULL, DNS_TYPE_SRV, DNS_CLASS_IN, false,
			 rr_append_handler, &req->srvl);

	if (!req->srvl.head) {
		if (!req->tp_selected) {
			if (transp_next_srv(req->sip, &req->tp)) {

				err = srv_lookup(req, req->host);
				if (err)
					goto fail;

				return;
			}

			if (!transp_first(req->sip, &req->tp)) {
				err = EPROTONOSUPPORT;
				goto fail;
			}
		}

		req->port = sip_transp_port(req->tp, 0);

		err = addr_lookup(req, req->host);
		if (err)
			goto fail;

		return;
	}

	dns_rrlist_sort(&req->srvl, DNS_TYPE_SRV, req->sortkey);

	dns_rrlist_apply(addl, NULL, DNS_QTYPE_ANY, DNS_CLASS_IN, false,
			 rr_cache_handler, req);

	err = request_next(req);
	if (err)
		goto fail;

	return;

 fail:
	terminate(req, err, NULL);
	mem_deref(req);
}


static void addr_handler(int err, const struct dnshdr *hdr, struct list *ansl,
			 struct list *authl, struct list *addl, void *arg)
{
	struct sip_request *req = arg;
	(void)hdr;
	(void)authl;
	(void)addl;

	dns_rrlist_apply2(ansl, NULL, DNS_TYPE_A, DNS_TYPE_AAAA, DNS_CLASS_IN,
			  false, rr_append_handler, &req->addrl);

	/* wait for other (A/AAAA) query to complete */
	if (req->dnsq || req->dnsq2)
		return;

	if (!req->addrl.head && !req->srvl.head) {
		err = err ? err : EDESTADDRREQ;
		goto fail;
	}

	dns_rrlist_sort_addr(&req->addrl, req->sortkey);

	err = request_next(req);
	if (err)
		goto fail;

	return;

 fail:
	terminate(req, err, NULL);
	mem_deref(req);
}


static int srv_lookup(struct sip_request *req, const char *domain)
{
	char name[256];

	if (re_snprintf(name, sizeof(name), "%s.%s",
			sip_transp_srvid(req->tp), domain) < 0)
		return ENOMEM;

	return dnsc_query(&req->dnsq, req->sip->dnsc, name, DNS_TYPE_SRV,
			  DNS_CLASS_IN, true, srv_handler, req);
}


static int addr_lookup(struct sip_request *req, const char *name)
{
	int err;

	if (sip_transp_supported(req->sip, req->tp, AF_INET)) {

		err = dnsc_query(&req->dnsq, req->sip->dnsc, name,
				 DNS_TYPE_A, DNS_CLASS_IN, true,
				 addr_handler, req);
		if (err)
			return err;
	}

#ifdef HAVE_INET6
	if (sip_transp_supported(req->sip, req->tp, AF_INET6)) {

		err = dnsc_query(&req->dnsq2, req->sip->dnsc, name,
				 DNS_TYPE_AAAA, DNS_CLASS_IN, true,
				 addr_handler, req);
		if (err)
			return err;
	}
#endif

	if (!req->dnsq && !req->dnsq2)
		return EPROTONOSUPPORT;

	return 0;
}


static int sip_request_alloc(struct sip_request **reqp,
		struct sip *sip, bool stateful, const char *met, int metl,
		const char *uri, int uril, const struct uri *route,
		enum sip_transp tp, struct mbuf *mb, size_t sortkey,
		sip_send_h *sendh, sip_resp_h *resph, void *arg)
{
	struct sip_request *req;
	struct pl pl;
	int err;

	if (!sip || !met || !uri || !route || !mb)
		return EINVAL;

	if (pl_strcasecmp(&route->scheme, "sip"))
		return ENOSYS;

	req = mem_zalloc(sizeof(*req), destructor);
	if (!req)
		return ENOMEM;

	list_append(&sip->reql, &req->le, req);

	err = str_ldup(&req->met, met, metl);
	if (err)
		goto out;

	err = str_ldup(&req->uri, uri, uril);
	if (err)
		goto out;

	if (msg_param_decode(&route->params, "maddr", &pl))
		pl = route->host;

	err = pl_strdup(&req->host, &pl);
	if (err)
		goto out;

	req->stateful = stateful;
	req->sortkey = sortkey;
	req->mb    = mem_ref(mb);
	req->sip   = sip;
	req->sendh = sendh;
	req->resph = resph;
	req->arg   = arg;

	if (tp != SIP_TRANSP_NONE) {
		req->tp = tp;
	}
	else if (!msg_param_decode(&route->params, "transport", &pl)) {

		req->tp = sip_transp_decode(&pl);
		if (req->tp  == SIP_TRANSP_NONE) {
			err = EPROTONOSUPPORT;
			goto out;
		}

		if (!sip_transp_supported(sip, req->tp, AF_UNSPEC)) {
			err = EPROTONOSUPPORT;
			goto out;
		}

		req->tp_selected = true;
	}
	else {
		if (!transp_first(sip, &req->tp)) {
			err = EPROTONOSUPPORT;
			goto out;
		}

		req->tp_selected = false;
	}

out:
	if (err)
		mem_deref(req);
	else if (reqp)
		*reqp = req;

	return err;
}


static int sip_request_send(struct sip_request *req, struct sip *sip,
			    const struct uri *route)
{
	struct sa dst;
	int err;

	if (!sa_set_str(&dst, req->host,
			sip_transp_port(req->tp, route->port))) {

		err = request(req, req->tp, &dst);
		if (!req->stateful) {
			mem_deref(req);
			return err;
		}
	}
	else if (route->port) {

		req->port = sip_transp_port(req->tp, route->port);
		err = addr_lookup(req, req->host);
	}
	else if (req->tp_selected) {

		err = srv_lookup(req, req->host);
	}
	else {
	        err = dnsc_query(&req->dnsq, sip->dnsc, req->host,
				 DNS_TYPE_NAPTR, DNS_CLASS_IN, true,
				 naptr_handler, req);
	}

	if (err)
		mem_deref(req);
	else if (req->reqp)
		*req->reqp = req;

	return err;
}


/**
 * Send a SIP request
 *
 * @param reqp     Pointer to allocated SIP request object
 * @param sip      SIP Stack
 * @param stateful Stateful client transaction
 * @param met      SIP Method string
 * @param metl     Length of SIP Method string
 * @param uri      Request URI
 * @param uril     Length of Request URI string
 * @param route    Next hop route URI
 * @param mb       Buffer containing SIP request
 * @param sortkey  Key for DNS record sorting
 * @param sendh    Send handler
 * @param resph    Response handler
 * @param arg      Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int sip_request(struct sip_request **reqp, struct sip *sip, bool stateful,
		const char *met, int metl, const char *uri, int uril,
		const struct uri *route, struct mbuf *mb, size_t sortkey,
		sip_send_h *sendh, sip_resp_h *resph, void *arg)
{
	struct sip_request *req = NULL;
	int err;

	if (!sip || !met || !uri || !route || !mb)
		return EINVAL;

	err = sip_request_alloc(&req, sip, stateful, met, metl, uri, uril,
				route, SIP_TRANSP_NONE, mb, sortkey, sendh,
				resph, arg);
	if (err)
		goto out;

	req->reqp = reqp;
	err = sip_request_send(req, sip, route);
 out:

	return err;
}


/**
 * Send a SIP request with formatted arguments
 *
 * @param reqp     Pointer to allocated SIP request object
 * @param sip      SIP Stack
 * @param stateful Stateful client transaction
 * @param met      Null-terminated SIP Method string
 * @param uri      Null-terminated Request URI string
 * @param route    Next hop route URI (optional)
 * @param auth     SIP authentication state
 * @param sendh    Send handler
 * @param resph    Response handler
 * @param arg      Handler argument
 * @param fmt      Formatted SIP headers and body
 *
 * @return 0 if success, otherwise errorcode
 */
int sip_requestf(struct sip_request **reqp, struct sip *sip, bool stateful,
		 const char *met, const char *uri, const struct uri *route,
		 struct sip_auth *auth, sip_send_h *sendh, sip_resp_h *resph,
		 void *arg, const char *fmt, ...)
{
	struct uri lroute;
	struct mbuf *mb;
	va_list ap;
	int err;

	if (!sip || !met || !uri || !fmt)
		return EINVAL;

	if (!route) {
		struct pl uripl;

		pl_set_str(&uripl, uri);

		err = uri_decode(&lroute, &uripl);
		if (err)
			return err;

		route = &lroute;
	}

	mb = mbuf_alloc(2048);
	if (!mb)
		return ENOMEM;

	err = mbuf_write_str(mb, "Max-Forwards: 70\r\n");

	if (auth)
		err |= sip_auth_encode(mb, auth, met, uri);

	if (err)
		goto out;

	va_start(ap, fmt);
	err = mbuf_vprintf(mb, fmt, ap);
	va_end(ap);

	if (err)
		goto out;

	mb->pos = 0;

	err = sip_request(reqp, sip, stateful, met, -1, uri, -1, route, mb,
			  (size_t)arg, sendh, resph, arg);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	return err;
}


/**
 * Send a SIP dialog request with formatted arguments
 *
 * @param reqp     Pointer to allocated SIP request object
 * @param sip      SIP Stack
 * @param stateful Stateful client transaction
 * @param met      Null-terminated SIP Method string
 * @param dlg      SIP Dialog state
 * @param cseq     CSeq number
 * @param auth     SIP authentication state
 * @param sendh    Send handler
 * @param resph    Response handler
 * @param arg      Handler argument
 * @param fmt      Formatted SIP headers and body
 *
 * @return 0 if success, otherwise errorcode
 */
int sip_drequestf(struct sip_request **reqp, struct sip *sip, bool stateful,
		  const char *met, struct sip_dialog *dlg, uint32_t cseq,
		  struct sip_auth *auth, sip_send_h *sendh, sip_resp_h *resph,
		  void *arg, const char *fmt, ...)
{
	struct sip_request *req;
	struct mbuf *mb;
	va_list ap;
	int err;

	if (!sip || !met || !dlg || !fmt)
		return EINVAL;

	mb = mbuf_alloc(2048);
	if (!mb)
		return ENOMEM;

	err = mbuf_write_str(mb, "Max-Forwards: 70\r\n");

	if (auth)
		err |= sip_auth_encode(mb, auth, met, sip_dialog_uri(dlg));

	err |= sip_dialog_encode(mb, dlg, cseq, met);

	if (sip->software)
		err |= mbuf_printf(mb, "User-Agent: %s\r\n", sip->software);

	if (err)
		goto out;

	va_start(ap, fmt);
	err = mbuf_vprintf(mb, fmt, ap);
	va_end(ap);

	if (err)
		goto out;

	mb->pos = 0;

	err = sip_request_alloc(&req, sip, stateful, met, -1,
				sip_dialog_uri(dlg), -1, sip_dialog_route(dlg),
				sip_dialog_tp(dlg),
				mb, sip_dialog_hash(dlg), sendh, resph, arg);
	if (err)
		goto out;

	req->reqp = reqp;
	err = sip_request_send(req, sip, sip_dialog_route(dlg));

out:
	mem_deref(mb);

	return err;
}


/**
 * Cancel a pending SIP Request
 *
 * @param req SIP Request
 */
void sip_request_cancel(struct sip_request *req)
{
	if (!req || req->canceled)
		return;

	req->canceled = true;

	if (!req->provrecv)
		return;

	(void)sip_ctrans_cancel(req->ct);
}


void sip_request_close(struct sip *sip)
{
	if (!sip)
		return;

	list_apply(&sip->reql, true, close_handler, NULL);
}


/**
 * Check if a SIP request loops
 *
 * @param ls    Loop state
 * @param scode Status code from SIP response
 *
 * @return True if loops, otherwise false
 */
bool sip_request_loops(struct sip_loopstate *ls, uint16_t scode)
{
	bool loop = false;

	if (!ls)
		return false;

	if (scode < 200) {
		return false;
	}
	else if (scode < 300) {
		ls->failc = 0;
	}
	else if (scode < 400) {
		loop = (++ls->failc >= 16);
	}
	else {
		switch (scode) {

		default:
			if (ls->last_scode == scode)
				loop = true;
			/*@fallthrough@*/
		case 401:
		case 407:
		case 491:
			if (++ls->failc >= 16)
				loop = true;
			break;
		}
	}

	ls->last_scode = scode;

	return loop;
}


/**
 * Reset the loop state
 *
 * @param ls Loop state
 */
void sip_loopstate_reset(struct sip_loopstate *ls)
{
	if (!ls)
		return;

	ls->last_scode = 0;
	ls->failc = 0;
}
