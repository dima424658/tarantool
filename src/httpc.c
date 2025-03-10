/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "httpc.h"

#include <assert.h>
#include <curl/curl.h>
#include "tt_static.h"
#include "fiber.h"
#include "errinj.h"

#define MAX_HEADER_LEN 8192

static_assert(MAX_HEADER_LEN < SMALL_STATIC_SIZE,
	      "HTTP header fits into the static buffer");

/** The HTTP headers that may be set automatically. */
#define HTTP_ACCEPT_HEADER	"Accept:"
#define HTTP_CONNECTION_HEADER	"Connection:"
#define HTTP_CONTENT_LENGTH_HEADER "Content-Length:"
#define HTTP_KEEP_ALIVE_HEADER	"Keep-Alive:"

/**
 * libcurl callback for CURLOPT_READFUNCTION
 * @see https://curl.haxx.se/libcurl/c/CURLOPT_READFUNCTION.html
 */
static size_t
curl_easy_io_read_cb(char *buffer, size_t size, size_t nitems, void *ctx)
{
	struct httpc_request *req = (struct httpc_request *)ctx;
	const size_t buffer_size = size * nitems;

	size_t ibuf_len = ibuf_used(&req->send);
	if (ibuf_len == 0) {
		fiber_cond_broadcast(&req->io_send_cond);
		if (req->io_send_closed)
			return 0;
		return CURL_READFUNC_PAUSE;
	}

	size_t read_len = ibuf_len < buffer_size ? ibuf_len : buffer_size;
	memcpy(buffer, req->send.rpos, read_len);
	ibuf_consume(&req->send, read_len);

	fiber_cond_broadcast(&req->io_send_cond);
	return read_len;
}

/**
 * libcurl callback for CURLOPT_WRITEFUNCTION
 * @see https://curl.haxx.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
 */
static size_t
curl_easy_write_cb(char *ptr, size_t size, size_t nmemb, void *ctx)
{
	struct httpc_request *req = (struct httpc_request *) ctx;
	const size_t bytes = size * nmemb;

	char *p = region_alloc(&req->recv, bytes);
	if (p == NULL) {
		diag_set(OutOfMemory, bytes, "ibuf", "httpc body");
		return 0;
	}

	memcpy(p, ptr, bytes);

	if (req->io)
		fiber_cond_signal(&req->io_recv_cond);

	return bytes;
}

/**
 * libcurl callback for CURLOPT_HEADERFUNCTION
 * @see https://curl.haxx.se/libcurl/c/CURLOPT_HEADERFUNCTION.html
 */
static size_t
curl_easy_header_cb(char *buffer, size_t size, size_t nitems, void *ctx)
{
	struct httpc_request *req = (struct httpc_request *) ctx;
	const size_t bytes = size * nitems;
	/*
	 * The callback is invoked for the headers of all responses received
	 * after initiating a request and not just the final response. Since
	 * we're only interested in the final response headers, we drop all
	 * accumulated headers on each redirect.
	 */
	long redirect_count;
	curl_easy_getinfo(req->curl_request.easy, CURLINFO_REDIRECT_COUNT,
			  &redirect_count);
	if (redirect_count > req->redirect_count) {
		assert(redirect_count == req->redirect_count + 1);
		req->redirect_count = redirect_count;
		region_reset(&req->resp_headers);
	}
	char *p = region_alloc(&req->resp_headers, bytes);
	if (p == NULL) {
		diag_set(OutOfMemory, bytes, "ibuf", "httpc header");
		return 0;
	}
	memcpy(p, buffer, bytes);
	return bytes;
}

int
httpc_env_create(struct httpc_env *env, int max_conns, int max_total_conns)
{
	memset(env, 0, sizeof(*env));
	mempool_create(&env->req_pool, &cord()->slabc,
			sizeof(struct httpc_request));

	return curl_env_create(&env->curl_env, max_conns, max_total_conns);
}

void
httpc_env_destroy(struct httpc_env *ctx)
{
	assert(ctx);

	curl_env_destroy(&ctx->curl_env);

	mempool_destroy(&ctx->req_pool);
}

struct httpc_request *
httpc_request_new(struct httpc_env *env, const char *method,
		  const char *url)
{
	struct httpc_request *req = mempool_alloc(&env->req_pool);
	if (req == NULL) {
		diag_set(OutOfMemory, sizeof(struct httpc_request),
			 "mempool", "httpc_request");
		return NULL;
	}
	memset(req, 0, sizeof(*req));
	req->env = env;
	req->set_connection_header = true;
	req->set_keep_alive_header = true;
	req->content_length = -1;

	region_create(&req->resp_headers, &cord()->slabc);
	region_create(&req->recv, &cord()->slabc);

	if (curl_request_create(&req->curl_request) != 0)
		return NULL;

	if (strcmp(method, "GET") == 0) {
		curl_easy_setopt(req->curl_request.easy, CURLOPT_HTTPGET, 1L);
	} else if (strcmp(method, "HEAD") == 0) {
		curl_easy_setopt(req->curl_request.easy, CURLOPT_NOBODY, 1L);
	} else if (strcmp(method, "POST") == 0 ||
		   strcmp(method, "PUT") == 0 ||
		   strcmp(method, "PATCH")) {
		/*
		 * Set CURLOPT_POSTFIELDS to "" and CURLOPT_POSTFIELDSSIZE 0
		 * to avoid the read callback in any cases even if user
		 * forgot to call httpc_set_body() or httpc_set_io() for
		 * POST request.
		 * @see https://curl.haxx.se/libcurl/c/CURLOPT_POSTFIELDS.html
		 */
		curl_easy_setopt(req->curl_request.easy, CURLOPT_POST, 1L);
		curl_easy_setopt(req->curl_request.easy, CURLOPT_POSTFIELDS, "");
		curl_easy_setopt(req->curl_request.easy, CURLOPT_POSTFIELDSIZE, 0);
		curl_easy_setopt(req->curl_request.easy, CURLOPT_CUSTOMREQUEST, method);
		req->set_accept_header = true;
	} else {
		curl_easy_setopt(req->curl_request.easy, CURLOPT_CUSTOMREQUEST, method);
	}

	curl_easy_setopt(req->curl_request.easy, CURLOPT_URL, url);

	curl_easy_setopt(req->curl_request.easy, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(req->curl_request.easy, CURLOPT_SSL_VERIFYPEER, 1);
	curl_easy_setopt(req->curl_request.easy, CURLOPT_WRITEFUNCTION,
			 curl_easy_write_cb);
	curl_easy_setopt(req->curl_request.easy, CURLOPT_HEADERFUNCTION,
			 curl_easy_header_cb);
	curl_easy_setopt(req->curl_request.easy, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(req->curl_request.easy, CURLOPT_NOSIGNAL, 1L);

	ibuf_create(&req->send, &cord()->slabc, 1);

	return req;
}

void
httpc_request_delete(struct httpc_request *req)
{
	if (req->headers != NULL)
		curl_slist_free_all(req->headers);

	curl_request_destroy(&req->curl_request);

	ibuf_destroy(&req->send);
	region_destroy(&req->resp_headers);
	region_destroy(&req->recv);

	if (req->io) {
		fiber_cond_destroy(&req->io_send_cond);
		fiber_cond_destroy(&req->io_recv_cond);
		ibuf_destroy(&req->io_recv);
	}

	mempool_free(&req->env->req_pool, req);
}

int
httpc_set_header(struct httpc_request *req, const char *fmt, ...)
{
	char *header = static_alloc(MAX_HEADER_LEN + 1);
	va_list ap;
	va_start(ap, fmt);
	int rc = vsnprintf(header, MAX_HEADER_LEN + 1, fmt, ap);
	va_end(ap);
	if (rc > MAX_HEADER_LEN) {
		diag_set(IllegalParams, "header is too large");
		return -1;
	}

	/**
	 * Update flags for automanaged headers: no need to
	 * set them automatically later.
	 */
	if (strncasecmp(header, HTTP_ACCEPT_HEADER,
			strlen(HTTP_ACCEPT_HEADER)) == 0)
		req->set_accept_header = false;
	else if (strncasecmp(header, HTTP_CONNECTION_HEADER,
		 strlen(HTTP_CONNECTION_HEADER)) == 0)
		req->set_connection_header = false;
	else if (strncasecmp(header, HTTP_CONTENT_LENGTH_HEADER,
			     strlen(HTTP_CONTENT_LENGTH_HEADER)) == 0) {
		const char *len = header + strlen(HTTP_CONTENT_LENGTH_HEADER);
		char *end = NULL;
		long length = strtol(len, &end, 10);
		if (*end != '\0' || length < 0) {
			diag_set(IllegalParams,
				 "Content-Length header value "
				 "must be a non-negative integer");
			return -1;
		}
		req->content_length = length;
	} else if (strncasecmp(header, HTTP_KEEP_ALIVE_HEADER,
			       strlen(HTTP_KEEP_ALIVE_HEADER)) == 0)
		req->set_keep_alive_header = false;

	struct curl_slist *l = curl_slist_append(req->headers, header);
	if (l == NULL) {
		diag_set(OutOfMemory, strlen(header), "curl", "http header");
		return -1;
	}
	req->headers = l;
	return 0;
}

int
httpc_set_body(struct httpc_request *req, const char *body, size_t size)
{
	ibuf_reset(&req->send);
	char *chunk = ibuf_alloc(&req->send, size);
	if (chunk == NULL) {
		diag_set(OutOfMemory, size, "ibuf", "http request body");
		return -1;
	}
	memcpy(chunk, body, size);

	curl_easy_setopt(req->curl_request.easy, CURLOPT_POSTFIELDS,
			 req->send.buf);
	curl_easy_setopt(req->curl_request.easy, CURLOPT_POSTFIELDSIZE, size);

	if (httpc_set_header(req, "Content-Length: %zu", size) != 0)
		return -1;

	return 0;
}

void
httpc_set_keepalive(struct httpc_request *req, long idle, long interval)
{
#if LIBCURL_VERSION_NUM >= 0x071900
	/* keep-alive is available starting from libcurl 7.25.0 */
	if (idle > 0 && interval > 0) {
		curl_easy_setopt(req->curl_request.easy, CURLOPT_TCP_KEEPALIVE, 1L);
		curl_easy_setopt(req->curl_request.easy, CURLOPT_TCP_KEEPIDLE, idle);
		curl_easy_setopt(req->curl_request.easy, CURLOPT_TCP_KEEPINTVL, interval);
		req->keep_alive_timeout = idle;
	}
#else
	(void) req;
	(void) idle;
	(void) interval;
#endif
}

void
httpc_set_low_speed_time(struct httpc_request *req, long low_speed_time)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_LOW_SPEED_TIME, low_speed_time);
}

void
httpc_set_low_speed_limit(struct httpc_request *req, long low_speed_limit)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_LOW_SPEED_LIMIT, low_speed_limit);
}

void
httpc_set_verbose(struct httpc_request *req, bool curl_verbose)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_VERBOSE,
			 (long) curl_verbose);
}

void
httpc_set_ca_path(struct httpc_request *req, const char *ca_path)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_CAPATH, ca_path);
}

void
httpc_set_ca_file(struct httpc_request *req, const char *ca_file)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_CAINFO, ca_file);
}

int
httpc_set_unix_socket(struct httpc_request *req, const char *unix_socket)
{
#ifdef CURL_VERSION_UNIX_SOCKETS
	curl_easy_setopt(req->curl_request.easy, CURLOPT_UNIX_SOCKET_PATH,
			 unix_socket);
	return 0;
#else
#pragma message "unix sockets not supported, please upgrade libcurl to 7.40.0"
	(void) req;
	(void) unix_socket;
	diag_set(IllegalParams, "tarantool was built without unix socket support,"
				" please upgrade libcurl to 7.40.0 and rebuild");
	return -1;
#endif
}

void
httpc_set_verify_host(struct httpc_request *req, long verify)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_SSL_VERIFYHOST, verify);
}

void
httpc_set_verify_peer(struct httpc_request *req, long verify)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_SSL_VERIFYPEER, verify);
}

void
httpc_set_ssl_key(struct httpc_request *req, const char *ssl_key)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_SSLKEY, ssl_key);
}

void
httpc_set_ssl_cert(struct httpc_request *req, const char *ssl_cert)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_SSLCERT, ssl_cert);
}

void
httpc_set_proxy(struct httpc_request *req, const char *proxy)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_PROXY, proxy);
}

void
httpc_set_proxy_port(struct httpc_request *req, long port)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_PROXYPORT, port);
}

void
httpc_set_proxy_user_pwd(struct httpc_request *req, const char *user_pwd)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_PROXYUSERPWD, user_pwd);
}

void
httpc_set_no_proxy(struct httpc_request *req, const char *no_proxy)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_NOPROXY, no_proxy);
}

void
httpc_set_interface(struct httpc_request *req, const char *interface)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_INTERFACE, interface);
}

void
httpc_set_follow_location(struct httpc_request *req, long follow)
{
	curl_easy_setopt(req->curl_request.easy, CURLOPT_FOLLOWLOCATION,
			 follow);
}

void
httpc_set_accept_encoding(struct httpc_request *req, const char *encoding)
{
/*
* CURLOPT_ACCEPT_ENCODING was called CURLOPT_ENCODING before
* libcurl 7.21.6.
*/
#if LIBCURL_VERSION_NUM >= 0x071506
	curl_easy_setopt(req->curl_request.easy, CURLOPT_ACCEPT_ENCODING,
			 encoding);
#else
	curl_easy_setopt(req->curl_request.easy, CURLOPT_ENCODING,
			 encoding);
#endif
}

/**
 * The callback to call after a CURL request is completed.
 */
static void
httpc_curl_done_handler(void *arg)
{
	struct httpc_request *req = (struct httpc_request *)arg;
	fiber_cond_broadcast(&req->io_recv_cond);
	fiber_cond_broadcast(&req->io_send_cond);
	req->io_send_closed = true;
}

int
httpc_set_io(struct httpc_request *req, const char *method)
{
	if (req->io) {
		diag_set(IllegalParams, "io: request is already io");
		return -1;
	}

	req->io = true;
	fiber_cond_create(&req->io_recv_cond);
	fiber_cond_create(&req->io_send_cond);
	ibuf_create(&req->io_recv, &cord()->slabc, 1);

	if (strcmp(method, "POST") == 0 ||
	    strcmp(method, "PUT") == 0 ||
	    strcmp(method, "PATCH") == 0) {
		curl_easy_setopt(req->curl_request.easy, CURLOPT_READDATA,
				 (void *)req);
		curl_easy_setopt(req->curl_request.easy, CURLOPT_READFUNCTION,
				 curl_easy_io_read_cb);
		curl_easy_setopt(req->curl_request.easy, CURLOPT_UPLOAD, 1L);
		if (req->content_length >= 0) {
			curl_easy_setopt(req->curl_request.easy,
					 CURLOPT_INFILESIZE,
					 req->content_length);
		}
		req->io_send = true;
		req->io_send_closed = false;
		req->io_headers_cond = &req->io_send_cond;
	} else {
		req->io_send = false;
		req->io_send_closed = true;
		req->io_headers_cond = &req->io_recv_cond;
	}

	req->curl_request.done_handler = httpc_curl_done_handler;
	req->curl_request.done_handler_arg = req;

	return 0;
}

ssize_t
httpc_request_io_read(struct httpc_request *req, char *buf, size_t len,
		      double timeout)
{
	if (!req->io) {
		diag_set(IllegalParams, "io: request must be io");
		return -1;
	}

	size_t ibuf_len = ibuf_used(&req->io_recv);
	size_t recv_len = region_used(&req->recv);
	double deadline = ev_monotonic_now(loop()) + timeout;
	while (req->curl_request.in_progress && timeout > 0 &&
	       recv_len + ibuf_len == 0) {
		if (fiber_cond_wait_deadline(&req->io_recv_cond, deadline) != 0)
			return -1;
		ibuf_len = ibuf_used(&req->io_recv);
		recv_len = region_used(&req->recv);
	}

	if (recv_len + ibuf_len == 0)
		return 0;

	if (ibuf_len < len && recv_len > (len - ibuf_len)) {
		size_t uncopy_part = recv_len - (len - ibuf_len);
		if (ibuf_capacity(&req->io_recv) < uncopy_part) {
			size_t reserve = uncopy_part - ibuf_len;
			if (ibuf_reserve(&req->io_recv, reserve) == NULL) {
				diag_set(OutOfMemory, reserve, "ibuf", "recv");
				return -1;
			}
		}
		if (region_join(&req->recv, recv_len) == NULL) {
			diag_set(OutOfMemory, recv_len, "region", "rep_body");
			return -1;
		}
	}

	size_t copied = ibuf_len < len ? ibuf_len : len;
	if (copied > 0) {
		memcpy(buf, req->io_recv.rpos, copied);
		if (copied == ibuf_len)
			ibuf_reset(&req->io_recv);
		else
			ibuf_consume(&req->io_recv, copied);
	}

	if (copied < len && recv_len > 0) {
		size_t remain = len - copied;
		char *recv = region_join(&req->recv, recv_len);
		if (recv == NULL) {
			diag_set(OutOfMemory, recv_len, "region", "rep_body");
			return -1;
		}
		if (remain > recv_len)
			remain = recv_len;
		memcpy(buf + copied, recv, remain);
		copied += remain;

		if (recv_len > remain) {
			const size_t tocopy = recv_len - remain;
			char *ptr = ibuf_alloc(&req->io_recv, tocopy);
			memcpy(ptr, recv + remain, tocopy);
		}

		region_truncate(&req->recv, 0);
	}

	return copied;
}

ssize_t
httpc_request_io_write(struct httpc_request *req, const char *ptr, size_t len,
		       double timeout)
{
	if (!req->io) {
		diag_set(IllegalParams, "io: request must be io");
		return -1;
	}

	if (!req->io_send) {
		diag_set(IllegalParams,
			 "io: HTTP request method with no body to send");
		return -1;
	}

	if (!req->curl_request.in_progress || req->io_send_closed)
		return 0;

	size_t ibuf_len = ibuf_used(&req->send);
	double deadline = ev_monotonic_now(loop()) + timeout;
	while (req->curl_request.in_progress && ibuf_len && timeout > 0) {
		if (fiber_cond_wait_deadline(&req->io_send_cond, deadline) != 0)
			return -1;

		fiber_cond_wait(&req->io_send_cond);
		ibuf_len = ibuf_used(&req->send);
	}

	if (ibuf_len) {
		if (req->curl_request.in_progress) {
			diag_set(TimedOut);
			return -1;
		}
		return 0;
	}

	if (len > 0) {
		ibuf_reset(&req->send);
		char *buf = ibuf_alloc(&req->send, len);
		memcpy(buf, ptr, len);
	} else {
		req->io_send_closed = true;
	}

	curl_easy_pause(req->curl_request.easy, CURLPAUSE_SEND_CONT);
	fiber_cond_wait_deadline(&req->io_send_cond, deadline);
	if (ibuf_used(&req->send)) {
		ibuf_reset(&req->send);
		return 0;
	}

	return len;
}

int
httpc_request_io_finish(struct httpc_request *req, double timeout)
{
	if (!req->io) {
		diag_set(IllegalParams, "io: request must be io");
		return -1;
	}

	if (req->curl_request.in_progress && !req->io_send_closed) {
		double ts = ev_monotonic_now(loop());
		timeout = timeout > 0 ? timeout : 0;
		if (httpc_request_io_write(req, NULL, 0, timeout) == 0)
			req->io_send_closed = true;
		timeout -= ev_monotonic_now(loop()) - ts;
	}

	timeout = timeout > 0 ? timeout : 0;
	if (req->status == 0 && httpc_request_finish(req, timeout))
		return -1;

	fiber_cond_broadcast(&req->io_send_cond);
	fiber_cond_broadcast(&req->io_recv_cond);
	return 0;
}

int
httpc_request_start(struct httpc_request *req, double timeout)
{
	struct httpc_env *env = req->env;
	if (req->set_accept_header &&
	    httpc_set_header(req, "Accept: */*") != 0)
		return -1;
	if (req->set_connection_header &&
	    httpc_set_header(req, "Connection: %s",
			     req->keep_alive_timeout > 0 ?
			     "Keep-Alive" : "close") != 0)
		return -1;
	if (req->set_keep_alive_header && req->keep_alive_timeout > 0 &&
	    httpc_set_header(req, "Keep-Alive: timeout=%ld",
			     req->keep_alive_timeout) != 0)
		return -1;

	curl_easy_setopt(req->curl_request.easy, CURLOPT_WRITEDATA, (void *) req);
	curl_easy_setopt(req->curl_request.easy, CURLOPT_HEADERDATA, (void *) req);
	curl_easy_setopt(req->curl_request.easy, CURLOPT_PRIVATE, (void *) &req->curl_request);
	curl_easy_setopt(req->curl_request.easy, CURLOPT_HTTPHEADER, req->headers);

	++env->stat.total_requests;

	if (curl_request_start(&req->curl_request, &env->curl_env) != CURLM_OK)
		return -1;

	if (req->io && req->curl_request.in_progress &&
	    fiber_cond_wait_timeout(req->io_headers_cond, timeout) != 0) {
		httpc_request_finish(req, 0);
		return -1;
	}

	if (req->io && !req->curl_request.in_progress)
		return httpc_request_finish(req, 0);

	return 0;
}

int
httpc_request_finish(struct httpc_request *req, double timeout)
{
	struct httpc_env *env = req->env;

	if (curl_request_finish(&req->curl_request, &env->curl_env, timeout) != CURLM_OK)
		return -1;

	ERROR_INJECT_RETURN(ERRINJ_HTTPC_EXECUTE);
	long longval = 0;
	switch (req->curl_request.code) {
	case CURLE_OK:
		curl_easy_getinfo(req->curl_request.easy, CURLINFO_RESPONSE_CODE, &longval);
		req->status = (int) longval;

		if (req->status >= 100 && req->status < 400)
			req->reason = "Ok";
		else
			req->reason = "Unknown";

		if (req->status == 200) {
			++env->stat.http_200_responses;
		} else {
			++env->stat.http_other_responses;
		}
		break;
#if LIBCURL_VERSION_NUM < 0x073e00
	case CURLE_SSL_CACERT: /* deprecated in libcurl 7.62.0 */
#endif
	case CURLE_PEER_FAILED_VERIFICATION:
		/* 495 SSL Certificate Error (nginx non-standard) */
		req->status = 495;
		req->reason = curl_easy_strerror(req->curl_request.code);
		++env->stat.failed_requests;
		break;
	case CURLE_OPERATION_TIMEDOUT:
		/* 408 Request Timeout (nginx non-standard) */
		req->status = 408;
		req->reason = curl_easy_strerror(req->curl_request.code);
		++env->stat.failed_requests;
		break;
	case CURLE_GOT_NOTHING:
		/* 444 No Response */
		req->status = 444;
		req->reason = curl_easy_strerror(req->curl_request.code);
		++env->stat.failed_requests;
		break;
	case CURLE_COULDNT_RESOLVE_PROXY:
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_CONNECT:
	case CURLE_WRITE_ERROR:
	case CURLE_BAD_CONTENT_ENCODING:
		/* 595 Connection Problem (AnyEvent non-standard) */
		req->status = 595;
		req->reason = curl_easy_strerror(req->curl_request.code);
		++env->stat.failed_requests;
		break;
	case CURLE_OUT_OF_MEMORY:
		diag_set(OutOfMemory, 0, "curl", "internal");
		++env->stat.failed_requests;
		return -1;
	default:
		curl_easy_getinfo(req->curl_request.easy, CURLINFO_OS_ERRNO, &longval);
		errno = longval ? longval : EINVAL;
		diag_set(SystemError, "curl: %s",
			 curl_easy_strerror(req->curl_request.code));
		++env->stat.failed_requests;
		return -1;
	}

	return 0;
}

int
httpc_execute(struct httpc_request *req, double timeout)
{
	int ret = httpc_request_start(req, timeout);
	if (ret != 0)
		return ret;
	return httpc_request_finish(req, timeout);
}
