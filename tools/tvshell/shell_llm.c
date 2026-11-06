// SPDX-License-Identifier: GPL-2.0-only
/*
 * Giri LLM backend selection and the LAN (remote) backend.
 *
 * Configuration lives in the persistent shell.conf (KEY=VALUE lines, shared
 * with other shell settings such as THEME):
 *
 *   LLM_MODE=device | remote          explicit, no fallback (default device)
 *   LLM_URL=http://192.168.68.101:8080 remote server base URL (plain HTTP)
 *   LLM_MODEL=qwen2.5:3b               model name sent to the server
 *   LLM_API_KEY=...                    optional "Authorization: Bearer"
 *   LLM_TIMEOUT_S=90                   whole-request timeout
 *   LLM_JSON_SCHEMA=1                  send response_format json_schema
 *   LLM_DEVICE_MODEL=/mnt/data/linux/giri/models/<file>.gguf
 *
 * The remote backend speaks the OpenAI-compatible chat completions API
 * (POST <url>/v1/chat/completions), served by llama.cpp llama-server, Ollama,
 * LM Studio and others. Requests run on a detached worker thread so the UI
 * loop never blocks; shell_llm_poll() hands the result back.
 */
#include "tvshell.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SHELL_CONF_PATH		"/mnt/data/linux/etc/shell.conf"
#define LLM_MAX_RESPONSE	(256 * 1024)
#define LLM_CONNECT_TIMEOUT_MS	5000

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/* Bounded copy that always terminates (silently truncates). */
static void scopy(char *dst, size_t dst_sz, const char *src)
{
	size_t n = strnlen(src, dst_sz - 1);

	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void copy_trim(char *dst, size_t dst_sz, const char *src)
{
	size_t len;

	while (*src == ' ' || *src == '\t')
		src++;
	len = strlen(src);
	while (len && (src[len - 1] == ' ' || src[len - 1] == '\t' ||
		       src[len - 1] == '\r' || src[len - 1] == '\n'))
		len--;
	if (len >= 2 && ((src[0] == '"' && src[len - 1] == '"') ||
			 (src[0] == '\'' && src[len - 1] == '\''))) {
		src++;
		len -= 2;
	}
	if (len >= dst_sz)
		len = dst_sz - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

const char *shell_llm_config_path(void)
{
	const char *p = getenv("TVSHELL_CONF");

	return (p && *p) ? p : SHELL_CONF_PATH;
}

void shell_llm_config_load(struct shell_llm_config *cfg)
{
	const char *path = shell_llm_config_path();
	char line[512];
	FILE *fp;

	memset(cfg, 0, sizeof(*cfg));
	cfg->mode = SHELL_LLM_DEVICE;
	cfg->timeout_s = 90;
	cfg->json_schema = true;

	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char key[32], val[256];
		char *eq = strchr(line, '=');

		if (line[0] == '#' || !eq)
			continue;
		*eq = '\0';
		copy_trim(key, sizeof(key), line);
		copy_trim(val, sizeof(val), eq + 1);

		if (!strcmp(key, "LLM_MODE")) {
			if (!strcasecmp(val, "remote"))
				cfg->mode = SHELL_LLM_REMOTE;
			else if (!strcasecmp(val, "device"))
				cfg->mode = SHELL_LLM_DEVICE;
			else
				fprintf(stderr, "tvshell: %s: LLM_MODE=%s is not "
					"device|remote, using device\n", path, val);
		} else if (!strcmp(key, "LLM_URL")) {
			scopy(cfg->url, sizeof(cfg->url), val);
		} else if (!strcmp(key, "LLM_MODEL")) {
			scopy(cfg->model, sizeof(cfg->model), val);
		} else if (!strcmp(key, "LLM_API_KEY")) {
			scopy(cfg->api_key, sizeof(cfg->api_key), val);
		} else if (!strcmp(key, "LLM_TIMEOUT_S")) {
			int t = atoi(val);

			if (t >= 5 && t <= 600)
				cfg->timeout_s = t;
		} else if (!strcmp(key, "LLM_JSON_SCHEMA")) {
			cfg->json_schema = !(val[0] == '0' || !strcasecmp(val, "off") ||
					     !strcasecmp(val, "false") || !strcasecmp(val, "no"));
		} else if (!strcmp(key, "LLM_DEVICE_MODEL")) {
			scopy(cfg->device_model, sizeof(cfg->device_model), val);
		}
	}
	fclose(fp);
}

/* "LAN server · 192.168.68.101:8080 · qwen2.5:3b" / "On-device · <file>" */
void shell_llm_describe(const struct shell_llm_config *cfg, char *buf, size_t sz)
{
	if (cfg->mode == SHELL_LLM_REMOTE) {
		const char *u = cfg->url;

		if (!strncmp(u, "http://", 7))
			u += 7;
		snprintf(buf, sz, "LAN server · %s%s%s", u[0] ? u : "(LLM_URL not set)",
			 cfg->model[0] ? " · " : "", cfg->model);
	} else {
		const char *m = strrchr(cfg->device_model, '/');

		m = m ? m + 1 : cfg->device_model;
		snprintf(buf, sz, "On-device · %s", m[0] ? m : "built-in responses");
	}
}

/* ------------------------------------------------------------------ */
/* Small JSON helpers                                                  */
/* ------------------------------------------------------------------ */

/* Append s to dst as a JSON string body (no surrounding quotes). */
static size_t json_escape(char *dst, size_t cap, size_t pos, const char *s)
{
	for (; *s && pos + 7 < cap; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			dst[pos++] = '\\';
			dst[pos++] = (char)c;
		} else if (c == '\n') {
			dst[pos++] = '\\';
			dst[pos++] = 'n';
		} else if (c < 0x20) {
			pos += (size_t)snprintf(dst + pos, cap - pos, "\\u%04x", c);
		} else {
			dst[pos++] = (char)c;
		}
	}
	dst[pos] = '\0';
	return pos;
}

static void put_utf8(char **o, char *end, unsigned cp)
{
	char tmp[4];
	int n, i;

	if (cp < 0x80) {
		tmp[0] = (char)cp; n = 1;
	} else if (cp < 0x800) {
		tmp[0] = (char)(0xc0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3f)); n = 2;
	} else if (cp < 0x10000) {
		tmp[0] = (char)(0xe0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		tmp[2] = (char)(0x80 | (cp & 0x3f)); n = 3;
	} else {
		tmp[0] = (char)(0xf0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); tmp[3] = (char)(0x80 | (cp & 0x3f)); n = 4;
	}
	for (i = 0; i < n && *o < end; i++)
		*(*o)++ = tmp[i];
}

static unsigned hex4(const char *p)
{
	unsigned v = 0;
	int i;

	for (i = 0; i < 4; i++) {
		char c = p[i];

		v <<= 4;
		if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
		else return 0xfffd;
	}
	return v;
}

/*
 * Decode the JSON string starting at p (pointing at the opening quote) into
 * dst. Returns a pointer past the closing quote, or NULL on malformed input.
 */
static const char *json_decode_string(const char *p, char *dst, size_t dst_sz)
{
	char *o = dst, *end = dst + dst_sz - 1;

	if (*p != '"')
		return NULL;
	for (p++; *p && *p != '"'; p++) {
		if (*p != '\\') {
			if (o < end)
				*o++ = *p;
			continue;
		}
		p++;
		switch (*p) {
		case 'n': if (o < end) *o++ = ' '; break;	/* one TV line */
		case 't': case 'r': if (o < end) *o++ = ' '; break;
		case 'b': case 'f': break;
		case 'u': {
			unsigned cp;

			if (!p[1] || !p[2] || !p[3] || !p[4])
				return NULL;
			cp = hex4(p + 1);
			p += 4;
			if (cp >= 0xd800 && cp < 0xdc00 && p[1] == '\\' && p[2] == 'u') {
				unsigned lo = hex4(p + 3);

				if (lo >= 0xdc00 && lo < 0xe000) {
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
					p += 6;
				}
			}
			put_utf8(&o, end, cp);
			break;
		}
		case '\0':
			return NULL;
		default:	/* \" \\ \/ */
			if (o < end)
				*o++ = *p;
		}
	}
	*o = '\0';
	return *p == '"' ? p + 1 : NULL;
}

/* Find "key": "<string>" at or after start; decode the value into dst. */
static bool json_get_string(const char *start, const char *key, char *dst, size_t dst_sz)
{
	char pat[40];
	const char *p = start;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	while ((p = strstr(p, pat))) {
		const char *q = p + strlen(pat);

		while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
			q++;
		if (*q == ':') {
			q++;
			while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
				q++;
			return json_decode_string(q, dst, dst_sz) != NULL;
		}
		p = q;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* HTTP client                                                         */
/* ------------------------------------------------------------------ */

struct llm_job {
	struct shell_llm_config cfg;
	char query[256];
	unsigned gen;
};

static pthread_mutex_t g_llm_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_llm_gen;
static bool g_llm_done;
static struct shell_llm_result g_llm_result;

static bool job_cancelled(const struct llm_job *job)
{
	return __atomic_load_n(&g_llm_gen, __ATOMIC_ACQUIRE) != job->gen;
}

static uint64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* http://host[:port][/prefix], host may be [v6] or [v6%iface] */
static bool parse_url(const char *url, char *host, size_t host_sz, char *port,
		      size_t port_sz, char *path, size_t path_sz, char *err, size_t err_sz)
{
	const char *p, *h_end, *rest;
	size_t hl;

	if (!strncasecmp(url, "https://", 8)) {
		snprintf(err, err_sz, "https is not supported, use http:// on the LAN");
		return false;
	}
	if (strncasecmp(url, "http://", 7)) {
		snprintf(err, err_sz, "LLM_URL must start with http://");
		return false;
	}
	p = url + 7;
	if (*p == '[') {
		h_end = strchr(p, ']');
		if (!h_end) {
			snprintf(err, err_sz, "bad IPv6 address in LLM_URL");
			return false;
		}
		hl = (size_t)(h_end - p - 1);
		if (hl >= host_sz)
			hl = host_sz - 1;
		memcpy(host, p + 1, hl);
		host[hl] = '\0';
		rest = h_end + 1;
	} else {
		h_end = p + strcspn(p, ":/");
		hl = (size_t)(h_end - p);
		if (hl >= host_sz)
			hl = host_sz - 1;
		memcpy(host, p, hl);
		host[hl] = '\0';
		rest = h_end;
	}
	if (!host[0]) {
		snprintf(err, err_sz, "no host in LLM_URL");
		return false;
	}
	if (*rest == ':') {
		size_t pl = strcspn(rest + 1, "/");

		if (pl == 0 || pl >= port_sz) {
			snprintf(err, err_sz, "bad port in LLM_URL");
			return false;
		}
		memcpy(port, rest + 1, pl);
		port[pl] = '\0';
		rest += 1 + pl;
	} else {
		snprintf(port, port_sz, "80");
	}
	/* base prefix without trailing '/', then the chat completions route */
	{
		size_t rl = strlen(rest);
		char prefix[160];

		while (rl && rest[rl - 1] == '/')
			rl--;
		if (rl >= sizeof(prefix))
			rl = sizeof(prefix) - 1;
		memcpy(prefix, rest, rl);
		prefix[rl] = '\0';
		if (rl >= 3 && !strcmp(prefix + rl - 3, "/v1"))
			snprintf(path, path_sz, "%s/chat/completions", prefix);
		else
			snprintf(path, path_sz, "%s/v1/chat/completions", prefix);
	}
	return true;
}

static int connect_with_timeout(const char *host, const char *port, int timeout_ms,
				char *err, size_t err_sz)
{
	struct addrinfo hints, *res = NULL, *ai;
	int rc, fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	rc = getaddrinfo(host, port, &hints, &res);
	if (rc) {
		snprintf(err, err_sz, "cannot resolve %s: %s", host, gai_strerror(rc));
		return -1;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		struct pollfd pfd;
		int soerr = 0, fl;
		socklen_t sl = sizeof(soerr);

		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0)
			continue;
		fl = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
			snprintf(err, err_sz, "connect %s:%s: %s", host, port, strerror(errno));
			close(fd);
			fd = -1;
			continue;
		}
		pfd.fd = fd;
		pfd.events = POLLOUT;
		if (poll(&pfd, 1, timeout_ms) != 1 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr) {
			snprintf(err, err_sz, "connect %s:%s: %s", host, port,
				 soerr ? strerror(soerr) : "timed out");
			close(fd);
			fd = -1;
			continue;
		}
		fcntl(fd, F_SETFL, fl);
		break;
	}
	freeaddrinfo(res);
	return fd;
}

static bool send_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		buf += n;
		len -= (size_t)n;
	}
	return true;
}

/* Decode a chunked body in place; returns the new length. */
static size_t dechunk(char *body, size_t len)
{
	char *r = body, *w = body, *end = body + len;

	while (r < end) {
		char *nl;
		unsigned long n = strtoul(r, &nl, 16);

		nl = strstr(r, "\r\n");
		if (!nl || n == 0)
			break;
		r = nl + 2;
		if ((size_t)(end - r) < n)
			n = (unsigned long)(end - r);
		memmove(w, r, n);
		w += n;
		r += n;
		if (r + 2 <= end && r[0] == '\r' && r[1] == '\n')
			r += 2;
	}
	return (size_t)(w - body);
}

static char *build_request_body(const struct llm_job *job)
{
	static const char *sys =
		"You are Giri, the assistant on a living-room TV. Answer the "
		"viewer's request for one TV screen. Reply with only a JSON object "
		"with these string fields: \"title\" (a headline, at most 64 "
		"characters), \"summary\" (one or two plain sentences, at most 250 "
		"characters, no markdown), \"next1\" and \"next2\" (two short "
		"follow-up questions the viewer might ask next, each at most 42 "
		"characters). No other text.";
	static const char *schema =
		",\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{"
		"\"name\":\"giri_screen\",\"strict\":true,\"schema\":{\"type\":\"object\","
		"\"properties\":{"
		"\"title\":{\"type\":\"string\",\"maxLength\":64},"
		"\"summary\":{\"type\":\"string\",\"maxLength\":250},"
		"\"next1\":{\"type\":\"string\",\"maxLength\":42},"
		"\"next2\":{\"type\":\"string\",\"maxLength\":42}},"
		"\"required\":[\"title\",\"summary\",\"next1\",\"next2\"],"
		"\"additionalProperties\":false}}}";
	const size_t cap = 4096;
	char *b = malloc(cap);
	size_t pos;

	if (!b)
		return NULL;
	pos = (size_t)snprintf(b, cap, "{");
	if (job->cfg.model[0]) {
		pos += (size_t)snprintf(b + pos, cap - pos, "\"model\":\"");
		pos = json_escape(b, cap, pos, job->cfg.model);
		pos += (size_t)snprintf(b + pos, cap - pos, "\",");
	}
	pos += (size_t)snprintf(b + pos, cap - pos,
		"\"stream\":false,\"temperature\":0.3,\"max_tokens\":320,"
		"\"messages\":[{\"role\":\"system\",\"content\":\"");
	pos = json_escape(b, cap, pos, sys);
	pos += (size_t)snprintf(b + pos, cap - pos,
		"\"},{\"role\":\"user\",\"content\":\"");
	pos = json_escape(b, cap, pos, job->query);
	pos += (size_t)snprintf(b + pos, cap - pos, "\"}]%s}",
				job->cfg.json_schema ? schema : "");
	return b;
}

/* Pull title/summary/next1/next2 out of the assistant message content. */
static void parse_content(const char *content, const char *query,
			  struct shell_llm_result *res)
{
	const char *obj = content;
	const char *think = strstr(content, "</think>");

	if (think)	/* reasoning models: skip the thinking block */
		obj = think + 8;
	obj = strchr(obj, '{');
	if (obj && json_get_string(obj, "summary", res->summary, sizeof(res->summary))) {
		json_get_string(obj, "title", res->title, sizeof(res->title));
		json_get_string(obj, "next1", res->next[0], sizeof(res->next[0]));
		json_get_string(obj, "next2", res->next[1], sizeof(res->next[1]));
	} else {
		/* model ignored the format: show its text as the summary */
		const char *t = think ? think + 8 : content;
		size_t i, j = 0;

		for (i = 0; t[i] && j + 1 < sizeof(res->summary); i++)
			res->summary[j++] = (t[i] == '\n' || t[i] == '\r') ? ' ' : t[i];
		res->summary[j] = '\0';
	}
	if (!res->title[0])
		scopy(res->title, sizeof(res->title), query);
	res->ok = res->summary[0] != '\0';
	if (!res->ok)
		snprintf(res->error, sizeof(res->error), "empty reply from the model");
}

static void run_job(const struct llm_job *job, struct shell_llm_result *res)
{
	char host[96], port[8], path[192], hdr[768];
	char *body = NULL, *resp = NULL;
	const uint64_t deadline = mono_ms() + (uint64_t)job->cfg.timeout_s * 1000;
	size_t blen, rlen = 0;
	int fd, hl, status = 0;

	if (!job->cfg.url[0]) {
		snprintf(res->error, sizeof(res->error), "LLM_URL is not set in %s",
			 shell_llm_config_path());
		return;
	}
	if (!parse_url(job->cfg.url, host, sizeof(host), port, sizeof(port),
		       path, sizeof(path), res->error, sizeof(res->error)))
		return;
	fd = connect_with_timeout(host, port, LLM_CONNECT_TIMEOUT_MS,
				  res->error, sizeof(res->error));
	if (fd < 0)
		return;

	body = build_request_body(job);
	if (!body) {
		snprintf(res->error, sizeof(res->error), "out of memory");
		goto out;
	}
	blen = strlen(body);
	{
		/* Host header: drop any IPv6 zone, bracket IPv6 literals */
		char hhost[96];
		size_t zl = strcspn(host, "%");

		snprintf(hhost, sizeof(hhost), strchr(host, ':') ? "[%.*s]" : "%.*s",
			 (int)zl, host);
		hl = snprintf(hdr, sizeof(hdr),
			      "POST %s HTTP/1.1\r\nHost: %s:%s\r\n"
			      "Content-Type: application/json\r\nAccept: application/json\r\n"
			      "%s%s%s"
			      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
			      path, hhost, port,
			      job->cfg.api_key[0] ? "Authorization: Bearer " : "",
			      job->cfg.api_key, job->cfg.api_key[0] ? "\r\n" : "", blen);
	}
	if (!send_all(fd, hdr, (size_t)hl) || !send_all(fd, body, blen)) {
		snprintf(res->error, sizeof(res->error), "send to %s failed: %s",
			 host, strerror(errno));
		goto out;
	}

	resp = malloc(LLM_MAX_RESPONSE + 1);
	if (!resp) {
		snprintf(res->error, sizeof(res->error), "out of memory");
		goto out;
	}
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		uint64_t now = mono_ms();
		ssize_t n;
		int pr;

		if (job_cancelled(job)) {
			snprintf(res->error, sizeof(res->error), "cancelled");
			goto out;
		}
		if (now >= deadline) {
			snprintf(res->error, sizeof(res->error),
				 "no reply from %s within %d s", host, job->cfg.timeout_s);
			goto out;
		}
		pr = poll(&pfd, 1, 250);	/* short slices so cancel is prompt */
		if (pr < 0 && errno != EINTR) {
			snprintf(res->error, sizeof(res->error), "poll: %s", strerror(errno));
			goto out;
		}
		if (pr <= 0)
			continue;
		n = recv(fd, resp + rlen, LLM_MAX_RESPONSE - rlen, 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			snprintf(res->error, sizeof(res->error), "recv: %s", strerror(errno));
			goto out;
		}
		if (n == 0)
			break;
		rlen += (size_t)n;
		if (rlen >= LLM_MAX_RESPONSE)
			break;
	}
	resp[rlen] = '\0';

	{
		char *hdr_end = strstr(resp, "\r\n\r\n");
		char *bodyp, *cl;
		size_t body_len;
		char *content;

		if (sscanf(resp, "HTTP/%*s %d", &status) != 1 || !hdr_end) {
			snprintf(res->error, sizeof(res->error), "not an HTTP reply from %s", host);
			goto out;
		}
		*hdr_end = '\0';
		bodyp = hdr_end + 4;
		body_len = rlen - (size_t)(bodyp - resp);
		for (cl = resp; *cl; cl++)
			*cl = (char)((*cl >= 'A' && *cl <= 'Z') ? *cl + 32 : *cl);
		if (strstr(resp, "transfer-encoding: chunked")) {
			body_len = dechunk(bodyp, body_len);
			bodyp[body_len] = '\0';
		}
		if (status != 200) {
			char msg[96];

			if (!json_get_string(bodyp, "message", msg, sizeof(msg)) &&
			    !json_get_string(bodyp, "error", msg, sizeof(msg)))
				snprintf(msg, sizeof(msg), "%.80s", bodyp);
			snprintf(res->error, sizeof(res->error), "HTTP %d: %s", status, msg);
			goto out;
		}
		{
			/* per-request buffer: a cancelled request may still be
			 * finishing on another thread */
			const size_t csz = 8192;
			const char *msgp = strstr(bodyp, "\"message\"");

			content = malloc(csz);
			if (!content) {
				snprintf(res->error, sizeof(res->error), "out of memory");
				goto out;
			}
			if (!msgp || !json_get_string(msgp, "content", content, csz)) {
				snprintf(res->error, sizeof(res->error),
					 "no message content in the server reply");
				free(content);
				goto out;
			}
			parse_content(content, job->query, res);
			free(content);
		}
	}
out:
	free(resp);
	free(body);
	close(fd);
}

static void *llm_thread(void *arg)
{
	struct llm_job *job = arg;
	struct shell_llm_result res;
	uint64_t t0 = mono_ms();

	memset(&res, 0, sizeof(res));
	run_job(job, &res);
	res.elapsed_ms = (int)(mono_ms() - t0);

	pthread_mutex_lock(&g_llm_lock);
	if (job->gen == g_llm_gen) {
		g_llm_result = res;
		g_llm_done = true;
	}
	pthread_mutex_unlock(&g_llm_lock);
	free(job);
	return NULL;
}

int shell_llm_start(const struct shell_llm_config *cfg, const char *query)
{
	struct llm_job *job = calloc(1, sizeof(*job));
	pthread_attr_t attr;
	pthread_t th;
	int rc;

	if (!job)
		return -1;
	job->cfg = *cfg;
	snprintf(job->query, sizeof(job->query), "%s", query ? query : "");

	pthread_mutex_lock(&g_llm_lock);
	job->gen = ++g_llm_gen;		/* invalidates any request in flight */
	g_llm_done = false;
	pthread_mutex_unlock(&g_llm_lock);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 128 * 1024);
	rc = pthread_create(&th, &attr, llm_thread, job);
	pthread_attr_destroy(&attr);
	if (rc) {
		free(job);
		return -1;
	}
	return 0;
}

bool shell_llm_poll(struct shell_llm_result *out)
{
	bool done;

	pthread_mutex_lock(&g_llm_lock);
	done = g_llm_done;
	if (done) {
		*out = g_llm_result;
		g_llm_done = false;
	}
	pthread_mutex_unlock(&g_llm_lock);
	return done;
}

void shell_llm_cancel(void)
{
	pthread_mutex_lock(&g_llm_lock);
	g_llm_gen++;
	g_llm_done = false;
	pthread_mutex_unlock(&g_llm_lock);
}
