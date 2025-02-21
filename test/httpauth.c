/**
 * @file httpauth.c HTTP Authentication Testcode
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include "test.h"


#define DEBUG_MODULE "httpauth"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


static int pl_equal(const char *name, const struct pl *a, const struct pl *b)
{
	static const struct pl plnil = PL("nil");
	int err;

	if ((a->p && !b->p) || (!a->p && b->p))
		err = EINVAL;
	else
		err = pl_cmp(a, b);

	if (err) {
		DEBUG_WARNING("%s mismatch: '%r' vs '%r'\n",
			      name,
			      a->p ? a : &plnil,
			      b->p ? b : &plnil);
	}

	return err;
}


static bool chall_equal(const struct httpauth_digest_chall *a,
			const struct httpauth_digest_chall *b)
{
	int err = 0;

	err |= pl_equal("realm",     &a->realm,     &b->realm);
	err |= pl_equal("nonce",     &a->nonce,     &b->nonce);
	err |= pl_equal("opaque",    &a->opaque,    &b->opaque);
	err |= pl_equal("stale",     &a->stale,     &b->stale);
	err |= pl_equal("algorithm", &a->algorithm, &b->algorithm);
	err |= pl_equal("qop",       &a->qop,       &b->qop);

	return err == 0;
}


static bool resp_equal(const struct httpauth_digest_resp *a,
		       const struct httpauth_digest_resp *b)
{
	int err = 0;

	err |= pl_equal("realm",     &a->realm,     &b->realm);
	err |= pl_equal("nonce",     &a->nonce,     &b->nonce);
	err |= pl_equal("response",  &a->response,  &b->response);
	err |= pl_equal("username",  &a->username,  &b->username);
	err |= pl_equal("uri",       &a->uri,       &b->uri);
	err |= pl_equal("nc",        &a->nc,        &b->nc);
	err |= pl_equal("cnonce",    &a->cnonce,    &b->cnonce);
	err |= pl_equal("qop",       &a->qop,       &b->qop);

	return err == 0;
}


int test_httpauth_chall(void)
{
	static const struct {
		const char *hval;
		struct httpauth_digest_chall chall;
		int err;
	} testv[] = {
		{
			"Digest realm=\"realm\","
			" nonce=\"4ee102da2fb730e04a26e8da913249b264f391c3\","
			" opaque=\"123\", stale=\"true\""
			" algorithm=\"MD5\""
			,
			{PL("realm"),
			 PL("4ee102da2fb730e04a26e8da913249b264f391c3"),
			 PL("123"),
			 PL("true"),
			 PL("MD5"),
			 PL_INIT},
			0
		},

		{
			"Digest realm=\"creytiv.com\","
			" nonce=\"9c916919cbc6ad7f54a4f64e5b5115074ee109fa\""
			", qop=\"auth\"",
			{PL("creytiv.com"),
			 PL("9c916919cbc6ad7f54a4f64e5b5115074ee109fa"),
			 PL_INIT, PL_INIT, PL_INIT,
			 PL("auth"),
			},
			0
		},

		{
			"Basic bogus",
			{PL_INIT, PL_INIT, PL_INIT, PL_INIT, PL_INIT, PL_INIT},
			EBADMSG
		},
	};
	size_t i;
	int err = 0;

	for (i=0; i<RE_ARRAY_SIZE(testv); i++) {

		struct httpauth_digest_chall chall;
		struct pl pl;
		int terr;

		pl_set_str(&pl, testv[i].hval);
		terr = httpauth_digest_challenge_decode(&chall, &pl);
		if (terr != testv[i].err) {
			DEBUG_WARNING("chall: expected error %d, got %d\n",
				      testv[i].err, terr);
			break;
		}

		if (terr)
			continue;

		if (!chall_equal(&testv[i].chall, &chall)) {
			DEBUG_WARNING("chall: test %d failed\n", i);
			err = EBADMSG;
			break;
		}
	}

	return err;
}


int test_httpauth_resp(void)
{
	static const struct {
		const char *hval;
		struct httpauth_digest_resp resp;
		struct pl method;
		uint8_t ha1[MD5_SIZE];
		int err;
	} testv[] = {
		{

			"Digest username=\"aeh\", realm=\"creytiv.com\","
			" nonce=\"9c916919cbc6ad7f54a4f64e5b5115074ee109fa\""
			", uri=\"sip:creytiv.com;transport=udp\","
			" response=\"bb996865add5a86217f39e1f369c29ea\","
			" cnonce=\"66a7a21e46ad8edd\", qop=auth, nc=00000002"
			,
			{PL("creytiv.com"),
			 PL("9c916919cbc6ad7f54a4f64e5b5115074ee109fa"),
			 PL("bb996865add5a86217f39e1f369c29ea"),
			 PL("aeh"),
			 PL("sip:creytiv.com;transport=udp"),
			 PL("00000002"),
			 PL("66a7a21e46ad8edd"),
			 PL("auth"),
			 NULL},
			PL("REGISTER"),
			"\x1c\x0a\x98\x61\x5b\x7b\x37\xc6"
			"\x94\x51\xae\xb6\x4b\x2f\x11\x02",
			0
		},
		{
			"Digest bogus tull",
			{PL_INIT, PL_INIT, PL_INIT, PL_INIT,
			 PL_INIT, PL_INIT, PL_INIT, PL_INIT, NULL},
			PL_INIT,
			"",
			EBADMSG
		},
	};
	size_t i;
	int err = 0;

	for (i=0; i<RE_ARRAY_SIZE(testv); i++) {

		struct httpauth_digest_resp resp;
		struct pl pl;
		int terr;

		pl_set_str(&pl, testv[i].hval);
		terr = httpauth_digest_response_decode(&resp, &pl);
		if (terr != testv[i].err) {
			DEBUG_WARNING("resp: expected error %d, got %d\n",
				      testv[i].err, terr);
			break;
		}

		if (terr)
			continue;

		if (!resp_equal(&testv[i].resp, &resp)) {
			DEBUG_WARNING("resp: test %d failed\n", i);
			err = EBADMSG;
			break;
		}

		err = httpauth_digest_response_auth(&resp, &testv[i].method,
						    testv[i].ha1);
		if (err) {
			if (err != ENOMEM) {
				DEBUG_WARNING("resp: auth failed %m\n", err);
			}
			break;
		}
	}

	return err;
}


int test_httpauth_basic_request(void) {
	static const struct {
		const char *hval;
		struct pl hval_response;
		const char *realm;
		const char *charset;
		const char *user;
		const char *passwd;
		int err;
		int auth_err;
	} testv[] = {
		{
			"Basic realm=\"/my/home\"",
			PL("Basic cmV0ZXN0OnJldGVzdHBhc3N3ZA=="),
			"/my/home", NULL, "retest", "retestpasswd", 0, 0
		},
		{
			"Basic realm=\"/my/home\", charset=\"UTF-8\"",
			PL("Basic cmV0ZXN0OnJldGVzdHBhc3N3ZOKCrA=="),
			"/my/home", "UTF-8", "retest",
			"retestpasswd\xe2\x82\xac",
			0, 0
		},
		{
			"Basic realm=\"/my/home\"",
			PL("Basic d3Jvbmc6Y3JlZGVudGlhbHM=="), "/my/home",
			NULL, "retest", "retestpasswd", 0, EACCES
		},
	};
	unsigned int i;
	int err = 0;

	for (i = 0; i < RE_ARRAY_SIZE(testv); ++i) {
		struct httpauth_basic_req *req = NULL;
		struct mbuf *mb = NULL;
		int terr = 0;
		int tauth_err = 0;

		terr = httpauth_basic_request(&req,
			testv[i].realm, testv[i].charset);
		if (terr == ENOMEM) {
			err = ENOMEM;
			break;
		}
		else if (terr != testv[i].err) {
			DEBUG_WARNING("basic req: expected error %d, got %m\n",
				testv[i].err, terr);
			err = terr;
			break;
		}

		if (str_casecmp(req->realm, testv[i].realm) != 0) {
			DEBUG_WARNING("basic req: expected realm %s, got %s\n",
				testv[i].realm, req->realm);
			err = EBADMSG;
			mem_deref(req);
			break;
		}

		if (testv[i].charset) {
			if (str_casecmp(req->charset, testv[i].charset) != 0) {
				DEBUG_WARNING("basic req: expected charset"
					"%s, got %s\n", testv[i].charset,
					req->charset);
				err = EBADMSG;
				mem_deref(req);
				break;
			}
		}

		mb = mbuf_alloc(512);
		if (!mb) {
			err = ENOMEM;
			mem_deref(req);
			break;
		}

		err = mbuf_printf(mb, "%H", httpauth_basic_request_print, req);
		if (err) {
			mem_deref(mb);
			mem_deref(req);
			break;
		}

		if (memcmp(testv[i].hval, mb->buf,
			str_len(testv[i].hval)) != 0) {
			DEBUG_WARNING("basic req: expected hval %s, got %s\n",
				testv[i].hval, mb->buf);
			err = EBADMSG;
			mem_deref(mb);
			mem_deref(req);
			break;
		}

		mem_deref(mb);
		tauth_err = httpauth_basic_verify(&testv[i].hval_response,
			testv[i].user, testv[i].passwd);
		if (tauth_err != testv[i].auth_err) {
			DEBUG_WARNING("basic req:"
				"authentication expected %d, got %d\n",
				testv[i].auth_err, tauth_err);
			mem_deref(req);
			break;
		}

		mem_deref(req);
	}

	return err;
}
