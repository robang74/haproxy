/*
 * include/haproxy/compression-t.h
 * This file defines everything related to compression.
 *
 * Copyright 2012 Exceliance, David Du Colombier <dducolombier@exceliance.fr>
                              William Lallemand <wlallemand@exceliance.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _HAPROXY_COMP_T_H
#define _HAPROXY_COMP_T_H

#if defined(USE_SLZ)
#ifdef USE_ZLIB
#error "Cannot build with both USE_SLZ and USE_ZLIB at the same time."
#endif
#include <import/slz.h>
#elif defined(USE_ZLIB)
#include <zlib.h>
#endif

#include <haproxy/buf-t.h>

/* Direction index */

#define COMP_DIR_REQ 0
#define COMP_DIR_RES 1

/* Compression flags */

#define COMP_FL_OFFLOAD		0x00000001 /* Compression offload */
#define COMP_FL_DIR_REQ		0x00000002 /* Compress requests */
#define COMP_FL_DIR_RES		0x00000004 /* Compress responses */

struct comp {
	struct comp_algo *algos_res; /* Algos available for response */
	struct comp_algo *algo_req;  /* Algo to use for request */
	struct comp_type *types_req; /* Types to be compressed for requests */
	struct comp_type *types_res; /* Types to be compressed for responses */
	unsigned int minsize_res;    /* Min response body size to be compressed */
	unsigned int minsize_req;    /* Min request body size to be compressed */
	unsigned int flags;
};

struct comp_ctx {
#if defined(USE_SLZ)
	struct slz_stream strm;
	const void *direct_ptr; /* NULL or pointer to beginning of data */
	int direct_len;         /* length of direct_ptr if not NULL */
	struct buffer queued;   /* if not NULL, data already queued */
#elif defined(USE_ZLIB)
	z_stream strm; /* zlib stream */
	void *zlib_deflate_state;
	void *zlib_window;
	void *zlib_prev;
	void *zlib_pending_buf;
	void *zlib_head;
#endif
	int cur_lvl;
};

/* Thanks to MSIE/IIS, the "deflate" name is ambiguous, as according to the RFC
 * it's a zlib-wrapped deflate stream, but MSIE only understands a raw deflate
 * stream. For this reason some people prefer to emit a raw deflate stream on
 * "deflate" and we'll need two algos for the same name, they are distinguished
 * with the config name.
 */
struct comp_algo {
	char *cfg_name;  /* config name */
	int cfg_name_len;

	char *ua_name;  /* name for the user-agent */
	int ua_name_len;

	int (*init)(struct comp_ctx **comp_ctx, int level);
	int (*add_data)(struct comp_ctx *comp_ctx, const char *in_data, int in_len, struct buffer *out);
	int (*flush)(struct comp_ctx *comp_ctx, struct buffer *out);
	int (*finish)(struct comp_ctx *comp_ctx, struct buffer *out);
	int (*end)(struct comp_ctx **comp_ctx);
	struct comp_algo *next;
};

struct comp_type {
	char *name;
	int name_len;
	struct comp_type *next;
};


#endif /* _HAPROXY_COMP_T_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */

