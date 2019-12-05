#include "config.h"
#include <assert.h>
#include <ccan/array_size/array_size.h>
#include <common/onion.h>
#include <common/sphinx.h>
#include <wire/gen_onion_wire.h>

/* BOLT #4:
 *
 * ## Legacy `hop_data` payload format
 *
 * The `hop_data` format is identified by a single `0x00`-byte length,
 * for backward compatibility.  Its payload is defined as:
 *
 * 1. type: `hop_data` (for `realm` 0)
 * 2. data:
 *    * [`short_channel_id`:`short_channel_id`]
 *    * [`u64`:`amt_to_forward`]
 *    * [`u32`:`outgoing_cltv_value`]
 *    * [`12*byte`:`padding`]
 */
static u8 *v0_hop(const tal_t *ctx,
		  const struct short_channel_id *scid,
		  struct amount_msat forward, u32 outgoing_cltv)
{
	const u8 padding[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			      0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	/* Prepend 0 byte for realm */
	u8 *buf = tal_arrz(ctx, u8, 1);
	towire_short_channel_id(&buf, scid);
	towire_u64(&buf, forward.millisatoshis); /* Raw: low-level serializer */
	towire_u32(&buf, outgoing_cltv);
	towire(&buf, padding, ARRAY_SIZE(padding));
	assert(tal_bytelen(buf) == 1 + 32);
	return buf;
}

static u8 *tlv_hop(const tal_t *ctx,
		   const struct tlv_tlv_payload *tlv)
{
	/* We can't have over 64k anyway */
	u8 *tlvs = tal_arr(ctx, u8, 3);

	towire_tlv_payload(&tlvs, tlv);

	switch (bigsize_put(tlvs, tal_bytelen(tlvs) - 3)) {
	case 1:
		/* Move over two unused bytes */
		memmove(tlvs + 1, tlvs + 3, tal_bytelen(tlvs) - 3);
		tal_resize(&tlvs, tal_bytelen(tlvs) - 2);
		return tlvs;
	case 3:
		return tlvs;
	}
	abort();
}

u8 *onion_nonfinal_hop(const tal_t *ctx,
		       bool use_tlv,
		       const struct short_channel_id *scid,
		       struct amount_msat forward,
		       u32 outgoing_cltv)
{
	if (use_tlv) {
		struct tlv_tlv_payload *tlv = tlv_tlv_payload_new(tmpctx);
		struct tlv_tlv_payload_amt_to_forward tlv_amt;
		struct tlv_tlv_payload_outgoing_cltv_value tlv_cltv;
		struct tlv_tlv_payload_short_channel_id tlv_scid;

		/* BOLT #4:
		 *
		 * The writer:
		 *  - MUST include `amt_to_forward` and `outgoing_cltv_value`
		 *    for every node.
		 *  - MUST include `short_channel_id` for every non-final node.
		 */
		tlv_amt.amt_to_forward = forward.millisatoshis; /* Raw: TLV convert */
		tlv_cltv.outgoing_cltv_value = outgoing_cltv;
		tlv_scid.short_channel_id = *scid;
		tlv->amt_to_forward = &tlv_amt;
		tlv->outgoing_cltv_value = &tlv_cltv;
		tlv->short_channel_id = &tlv_scid;

		return tlv_hop(ctx, tlv);
	} else {
		return v0_hop(ctx, scid, forward, outgoing_cltv);
	}
}

u8 *onion_final_hop(const tal_t *ctx,
		    bool use_tlv,
		    struct amount_msat forward,
		    u32 outgoing_cltv,
		    struct amount_msat total_msat,
		    const struct secret *payment_secret)
{
	/* These go together! */
	if (!payment_secret)
		assert(amount_msat_eq(total_msat, forward));

	if (use_tlv) {
		struct tlv_tlv_payload *tlv = tlv_tlv_payload_new(tmpctx);
		struct tlv_tlv_payload_amt_to_forward tlv_amt;
		struct tlv_tlv_payload_outgoing_cltv_value tlv_cltv;
#if EXPERIMENTAL_FEATURES
		struct tlv_tlv_payload_payment_data tlv_pdata;
#endif

		/* BOLT #4:
		 *
		 * The writer:
		 *  - MUST include `amt_to_forward` and `outgoing_cltv_value`
		 *    for every node.
		 *...
		 *  - MUST NOT include `short_channel_id` for the final node.
		 */
		tlv_amt.amt_to_forward = forward.millisatoshis; /* Raw: TLV convert */
		tlv_cltv.outgoing_cltv_value = outgoing_cltv;
		tlv->amt_to_forward = &tlv_amt;
		tlv->outgoing_cltv_value = &tlv_cltv;

#if EXPERIMENTAL_FEATURES
		if (payment_secret) {
			tlv_pdata.payment_secret = *payment_secret;
			tlv_pdata.total_msat = total_msat.millisatoshis; /* Raw: TLV convert */
			tlv->payment_data = &tlv_pdata;
		}
#else
		/* Wihtout EXPERIMENTAL_FEATURES, we can't send payment_secret */
		if (payment_secret)
			return NULL;
#endif
		return tlv_hop(ctx, tlv);
	} else {
		static struct short_channel_id all_zero_scid;
		/* No payment secrets in legacy format. */
		if (payment_secret)
			return NULL;
		return v0_hop(ctx, &all_zero_scid, forward, outgoing_cltv);
	}
}

/* Returns true if valid, and fills in type. */
static bool pull_payload_length(const u8 **cursor,
				size_t *max,
				enum onion_payload_type *type,
				size_t *len)
{
	/* *len will incorporate bytes we read from cursor */
	const u8 *start = *cursor;

	/* BOLT #4:
	 *
	 * The `length` field determines both the length and the format of the
	 * `hop_payload` field; the following formats are defined:
	 */
	*len = fromwire_bigsize(cursor, max);
	if (!cursor)
		return false;

	/* BOLT #4:
	 * - Legacy `hop_data` format, identified by a single `0x00` byte for
	 *   length. In this case the `hop_payload_length` is defined to be 32
	 *   bytes.
	 */
	if (*len == 0) {
		if (type)
			*type = ONION_V0_PAYLOAD;
		assert(*cursor - start == 1);
		*len = 1 + 32;
		return true;
	}

#if !EXPERIMENTAL_FEATURES
	/* Only handle legacy format */
	return false;
#else
	/* BOLT #4:
	 * - `tlv_payload` format, identified by any length over `1`. In this
	 *   case the `hop_payload_length` is equal to the numeric value of
	 *   `length`.
	 */
	if (*len > 1) {
		/* It's still invalid if it claims to be too long! */
		if (*len > ROUTING_INFO_SIZE - HMAC_SIZE)
			return false;

		if (type)
			*type = ONION_TLV_PAYLOAD;
		*len += (*cursor - start);
		return true;
	}

	return false;
#endif /* EXPERIMENTAL_FEATURES */
}

size_t onion_payload_length(const u8 *raw_payload,
			    bool *valid,
			    enum onion_payload_type *type)
{
	size_t max = ROUTING_INFO_SIZE, len;
	*valid = pull_payload_length(&raw_payload, &max, type, &len);

	if (!*valid)
		return ROUTING_INFO_SIZE;

	return len;
}

struct onion_contents *onion_decode(const tal_t *ctx,
				    const struct route_step *rs)
{
	struct onion_contents *c = tal(ctx, struct onion_contents);
	const u8 *cursor = rs->raw_payload;
	size_t max = tal_bytelen(cursor), len;
	struct tlv_tlv_payload *tlv;

	if (!pull_payload_length(&cursor, &max, &c->type, &len))
		return tal_free(c);

	switch (c->type) {
	case ONION_V0_PAYLOAD:
		c->type = ONION_V0_PAYLOAD;
		c->forward_channel = tal(c, struct short_channel_id);
		fromwire_short_channel_id(&cursor, &len, c->forward_channel);
		c->amt_to_forward = fromwire_amount_msat(&cursor, &len);
		c->outgoing_cltv = fromwire_u32(&cursor, &len);
		c->payment_secret = NULL;

		if (rs->nextcase == ONION_FORWARD) {
			c->total_msat = NULL;
		} else {
			/* BOLT-e36f7b6517e1173dcbd49da3b516cfe1f48ae556 #4:
			 * - if it is the final node:
			 *   - MUST treat `total_msat` as if it were equal to
			 *     `amt_to_forward` if it is not present. */
			c->total_msat = tal_dup(c, struct amount_msat,
						&c->amt_to_forward);
		}
		return c;

	case ONION_TLV_PAYLOAD:
		tlv = tal(c, struct tlv_tlv_payload);
		if (!fromwire_tlv_payload(&cursor, &len, tlv))
			return tal_free(c);
		if (!tlv_payload_is_valid(tlv, NULL))
			return tal_free(c);

		/* BOLT #4:
		 *
		 * The reader:
		 *   - MUST return an error if `amt_to_forward` or
		 *     `outgoing_cltv_value` are not present.
		 */
		if (!tlv->amt_to_forward || !tlv->outgoing_cltv_value)
			return tal_free(c);

		amount_msat_from_u64(&c->amt_to_forward,
				     tlv->amt_to_forward->amt_to_forward);
		c->outgoing_cltv = tlv->outgoing_cltv_value->outgoing_cltv_value;

		/* BOLT #4:
		 *
		 * The writer:
		 *...
		 *  - For every non-final node:
		 *    - MUST include `short_channel_id`
		 */
		if (rs->nextcase == ONION_FORWARD) {
			if (!tlv->short_channel_id)
				return tal_free(c);
			c->forward_channel = tal(c, struct short_channel_id);
			*c->forward_channel
				= tlv->short_channel_id->short_channel_id;
			c->total_msat = NULL;
		} else {
			c->forward_channel = NULL;
			/* BOLT-e36f7b6517e1173dcbd49da3b516cfe1f48ae556 #4:
			 * - if it is the final node:
			 *   - MUST treat `total_msat` as if it were equal to
			 *     `amt_to_forward` if it is not present. */
			c->total_msat = tal_dup(c, struct amount_msat,
						&c->amt_to_forward);
		}

		c->payment_secret = NULL;

#if EXPERIMENTAL_FEATURES
		if (tlv->payment_data) {
			c->payment_secret = tal_dup(c, struct secret,
						    &tlv->payment_data->payment_secret);
			tal_free(c->total_msat);
			c->total_msat = tal(c, struct amount_msat);
			c->total_msat->millisatoshis /* Raw: tu64 on wire */
				= tlv->payment_data->total_msat;
		}
#endif
		tal_free(tlv);
		return c;
	}

	/* You said it was a valid type! */
	abort();
}
