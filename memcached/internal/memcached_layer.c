#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>

#include <tarantool/module.h>

#include <msgpuck/msgpuck.h>
#include <small/obuf.h>

#include "error.h"
#include "memcached.h"
#include "memcached_layer.h"
#include "utils.h"
/*
 * default exptime is 30*24*60*60 seconds
 * \* 1000000 to convert it to usec (need this precision)
 **/
#define MAX_EXPTIME (30*24*60*60LL)
#define INF_EXPTIME UINT64_MAX

uint64_t
convert_exptime (uint64_t exptime)
{
	if (exptime == 0) return INF_EXPTIME; /* 0 means never expire */
	if (exptime <= MAX_EXPTIME)
		exptime = fiber_time64() + exptime * 1000000;
	else
		exptime = exptime * 1000000;
	return exptime;
}

static inline int
is_expired (uint64_t exptime, uint64_t time, uint64_t flush)
{
	(void )time;
	uint64_t curtime = fiber_time64();
	/* Expired by TTL or FLUSH */
	if (exptime <= curtime || (flush <= curtime && time <= flush))
		return 1;
	return 0;
}

int
is_expired_tuple(struct memcached_service *p, box_tuple_t *tuple)
{
	uint64_t flush = p->flush;
	const char *pos  = box_tuple_field(tuple, 1);
	uint64_t exptime = mp_decode_uint(&pos);
	uint64_t time    = mp_decode_uint(&pos);
	return is_expired(exptime, time, flush);
}

int
memcached_tuple_set(struct memcached_connection *con,
		    const char *kpos, uint32_t klen, uint64_t expire,
		    const char *vpos, uint32_t vlen, uint64_t cas,
		    uint32_t flags)
{
	(void )con;
	uint64_t time = fiber_time64();
	uint32_t len = mp_sizeof_array(6)      +
		       mp_sizeof_str  (klen)   +
		       mp_sizeof_uint (expire) +
		       mp_sizeof_uint (time)   +
		       mp_sizeof_str  (vlen)   +
		       mp_sizeof_uint (cas)    +
		       mp_sizeof_uint (flags);
	char *begin  = (char *)box_txn_alloc(len);
	if (begin == NULL) {
		memcached_error_ENOMEM(len, "tuple");
		return -1;
	}
	char *end = mp_encode_array(begin, 6);
	      end = mp_encode_str  (end, kpos, klen);
	      end = mp_encode_uint (end, expire);
	      end = mp_encode_uint (end, time);
	      end = mp_encode_str  (end, vpos, vlen);
	      end = mp_encode_uint (end, cas);
	      end = mp_encode_uint (end, flags);
	assert(end <= begin + len);
	return box_replace(con->cfg->space_id, begin, end, NULL);
}

int
memcached_tuple_get(struct memcached_connection *con,
		    const char *key, uint32_t key_len,
		    box_tuple_t **tuple)
{
	/* Create key for getting previous tuple from space */
	uint32_t len = mp_sizeof_array(1) +
		       mp_sizeof_str  (key_len);
	char *begin  = (char *)box_txn_alloc(len);
	if (begin == NULL) {
		memcached_error_ENOMEM(len, "key");
		return -1;
	}
	char *end = NULL;
	end = mp_encode_array(begin, 1);
	end = mp_encode_str  (end, key, key_len);
	assert(end <= begin + len);

	/* Get tuple from space */
	if (box_index_get(con->cfg->space_id, 0, begin, end, tuple) == -1) {
		return -1;
	}
	return 0;
}
