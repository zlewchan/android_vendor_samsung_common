/*
 * Copyright (C) 2008-2013 Tobias Brunner
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <openssl/base.h>

#ifndef OPENSSL_NO_EC

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/objects.h>
#include <openssl/bn.h>

#include "openssl_ec_diffie_hellman.h"
#include "openssl_util.h"

#include <utils/debug.h>

typedef struct private_openssl_ec_diffie_hellman_t private_openssl_ec_diffie_hellman_t;

/**
 * Private data of an openssl_ec_diffie_hellman_t object.
 */
struct private_openssl_ec_diffie_hellman_t {
	/**
	 * Public openssl_ec_diffie_hellman_t interface.
	 */
	openssl_ec_diffie_hellman_t public;

	/**
	 * Diffie Hellman group number.
	 */
	diffie_hellman_group_t group;

	/**
	 * EC private (public) key
	 */
	EC_KEY *key;

	/**
	 * EC group
	 */
	const EC_GROUP *ec_group;

	/**
	 * Other public key
	 */
	EC_POINT *pub_key;

	/**
	 * Shared secret
	 */
	chunk_t shared_secret;

	/**
	 * True if shared secret is computed
	 */
	bool computed;
};

/**
 * Convert a chunk to an EC_POINT (which must already exist). The x and y
 * coordinates of the point have to be concatenated in the chunk.
 */
static bool chunk2ecp(const EC_GROUP *group, chunk_t chunk, EC_POINT *point)
{
	BN_CTX *ctx;
	BIGNUM *x, *y;
	bool ret = FALSE;

	ctx = BN_CTX_new();
	if (!ctx)
	{
		return FALSE;
	}

	BN_CTX_start(ctx);
	x = BN_CTX_get(ctx);
	y = BN_CTX_get(ctx);
	if (!x || !y)
	{
		goto error;
	}

	if (!openssl_bn_split(chunk, x, y))
	{
		goto error;
	}

	if (!EC_POINT_set_affine_coordinates_GFp(group, point, x, y, ctx))
	{
		goto error;
	}

	if (!EC_POINT_is_on_curve(group, point, ctx))
	{
		goto error;
	}

	ret = TRUE;
error:
	BN_CTX_end(ctx);
	BN_CTX_free(ctx);
	return ret;
}

/**
 * Convert an EC_POINT to a chunk by concatenating the x and y coordinates of
 * the point. This function allocates memory for the chunk.
 */
static bool ecp2chunk(const EC_GROUP *group, const EC_POINT *point,
					  chunk_t *chunk, bool x_coordinate_only)
{
	BN_CTX *ctx;
	BIGNUM *x, *y;
	bool ret = FALSE;

	ctx = BN_CTX_new();
	if (!ctx)
	{
		return FALSE;
	}

	BN_CTX_start(ctx);
	x = BN_CTX_get(ctx);
	y = BN_CTX_get(ctx);
	if (!x || !y)
	{
		goto error;
	}

	if (!EC_POINT_get_affine_coordinates_GFp(group, point, x, y, ctx))
	{
		goto error;
	}

	if (x_coordinate_only)
	{
		y = NULL;
	}
	if (!openssl_bn_cat(EC_FIELD_ELEMENT_LEN(group), x, y, chunk))
	{
		goto error;
	}

	ret = TRUE;
error:
	BN_CTX_end(ctx);
	BN_CTX_free(ctx);
	return ret;
}

/**
 * Compute the shared secret.
 *
 * We cannot use the function ECDH_compute_key() because that returns only the
 * x coordinate of the shared secret point (which is defined, for instance, in
 * 'NIST SP 800-56A').
 * However, we need both coordinates as RFC 4753 says: "The Diffie-Hellman
 *   public value is obtained by concatenating the x and y values. The format
 *   of the Diffie-Hellman shared secret value is the same as that of the
 *   Diffie-Hellman public value."
 */
static bool compute_shared_key(private_openssl_ec_diffie_hellman_t *this,
							   chunk_t *shared_secret)
{
	const BIGNUM *priv_key;
	EC_POINT *secret = NULL;
	bool x_coordinate_only, ret = FALSE;

	priv_key = EC_KEY_get0_private_key(this->key);
	if (!priv_key)
	{
		goto error;
	}

	secret = EC_POINT_new(this->ec_group);
	if (!secret)
	{
		goto error;
	}

	if (!EC_POINT_mul(this->ec_group, secret, NULL, this->pub_key, priv_key, NULL))
	{
		goto error;
	}

	/*
	 * The default setting ecp_x_coordinate_only = TRUE
	 * applies the following errata for RFC 4753:
	 * http://www.rfc-editor.org/errata_search.php?eid=9
	 */
	x_coordinate_only = lib->settings->get_bool(lib->settings,
									"%s.ecp_x_coordinate_only", TRUE, lib->ns);
	if (!ecp2chunk(this->ec_group, secret, shared_secret, x_coordinate_only))
	{
		goto error;
	}

	ret = TRUE;
error:
	if (secret)
	{
		EC_POINT_clear_free(secret);
	}
	return ret;
}

METHOD(diffie_hellman_t, set_other_public_value, bool,
	private_openssl_ec_diffie_hellman_t *this, chunk_t value)
{
	if (!diffie_hellman_verify_value(this->group, value))
	{
		return FALSE;
	}

	if (!chunk2ecp(this->ec_group, value, this->pub_key))
	{
		DBG1(DBG_LIB, "ECDH public value is malformed");
		return FALSE;
	}

	chunk_clear(&this->shared_secret);

	if (!compute_shared_key(this, &this->shared_secret)) {
		DBG1(DBG_LIB, "ECDH shared secret computation failed");
		return FALSE;
	}

	this->computed = TRUE;
	return TRUE;
}

METHOD(diffie_hellman_t, get_my_public_value, bool,
	private_openssl_ec_diffie_hellman_t *this,chunk_t *value)
{
	ecp2chunk(this->ec_group, EC_KEY_get0_public_key(this->key), value, FALSE);
	return TRUE;
}

METHOD(diffie_hellman_t, set_private_value, bool,
	private_openssl_ec_diffie_hellman_t *this, chunk_t value)
{
	EC_POINT *pub = NULL;
	BIGNUM *priv = NULL;
	bool ret = FALSE;

	priv = BN_bin2bn(value.ptr, value.len, NULL);
	if (!priv)
	{
		goto error;
	}
	pub = EC_POINT_new(EC_KEY_get0_group(this->key));
	if (!pub)
	{
		goto error;
	}
	if (EC_POINT_mul(this->ec_group, pub, priv, NULL, NULL, NULL) != 1)
	{
		goto error;
	}
	if (EC_KEY_set_private_key(this->key, priv) != 1)
	{
		goto error;
	}
	if (EC_KEY_set_public_key(this->key, pub) != 1)
	{
		goto error;
	}
	ret = TRUE;

error:
	if (pub)
	{
		EC_POINT_free(pub);
	}
	if (priv)
	{
		BN_free(priv);
	}
	return ret;
}

METHOD(diffie_hellman_t, get_shared_secret, bool,
	private_openssl_ec_diffie_hellman_t *this, chunk_t *secret)
{
	if (!this->computed)
	{
		return FALSE;
	}
	*secret = chunk_clone(this->shared_secret);
	return TRUE;
}

METHOD(diffie_hellman_t, get_dh_group, diffie_hellman_group_t,
	private_openssl_ec_diffie_hellman_t *this)
{
	return this->group;
}

METHOD(diffie_hellman_t, destroy, void,
	private_openssl_ec_diffie_hellman_t *this)
{
	if (this->pub_key)
	{
		EC_POINT_clear_free(this->pub_key);
	}
	if (this->key)
	{
		EC_KEY_free(this->key);
	}
	chunk_clear(&this->shared_secret);
	free(this);
}

/**
 * ECC Brainpool curves are not available in OpenSSL releases < 1.0.2, but we
 * don't check the version in case somebody backported them.
 */
#if (!defined(NID_brainpoolP224r1) || !defined(NID_brainpoolP256r1) || \
	 !defined(NID_brainpoolP384r1) || !defined(NID_brainpoolP512r1))

/**
 * Parameters for ECC Brainpool curves
 */
typedef struct {
	/** DH group */
	diffie_hellman_group_t group;

	/** The prime p specifying the base field */
	const chunk_t p;

	/** Coefficient a of the elliptic curve E: y^2 = x^3 + ax + b (mod p) */
	const chunk_t a;

	/** Coefficient b */
	const chunk_t b;

	/** x coordinate of base point G (a point in E of prime order) */
	const chunk_t x;

	/** y coordinate of base point G */
	const chunk_t y;

	/** Prime order q of the group generated by G */
	const chunk_t q;

} bp_curve;

/**
 * List of ECC Brainpool curves
 */
static bp_curve bp_curves[] = {
	{
		/* ECC Brainpool 224-bit curve (RFC 5639), brainpoolP224r1 */
		.group = ECP_224_BP,
		.p = chunk_from_chars(
			0xD7,0xC1,0x34,0xAA,0x26,0x43,0x66,0x86,0x2A,0x18,0x30,0x25,0x75,0xD1,0xD7,0x87,
			0xB0,0x9F,0x07,0x57,0x97,0xDA,0x89,0xF5,0x7E,0xC8,0xC0,0xFF),
		.a = chunk_from_chars(
			0x68,0xA5,0xE6,0x2C,0xA9,0xCE,0x6C,0x1C,0x29,0x98,0x03,0xA6,0xC1,0x53,0x0B,0x51,
			0x4E,0x18,0x2A,0xD8,0xB0,0x04,0x2A,0x59,0xCA,0xD2,0x9F,0x43),
		.b = chunk_from_chars(
			0x25,0x80,0xF6,0x3C,0xCF,0xE4,0x41,0x38,0x87,0x07,0x13,0xB1,0xA9,0x23,0x69,0xE3,
			0x3E,0x21,0x35,0xD2,0x66,0xDB,0xB3,0x72,0x38,0x6C,0x40,0x0B),
		.x = chunk_from_chars(
			0x0D,0x90,0x29,0xAD,0x2C,0x7E,0x5C,0xF4,0x34,0x08,0x23,0xB2,0xA8,0x7D,0xC6,0x8C,
			0x9E,0x4C,0xE3,0x17,0x4C,0x1E,0x6E,0xFD,0xEE,0x12,0xC0,0x7D),
		.y = chunk_from_chars(
			0x58,0xAA,0x56,0xF7,0x72,0xC0,0x72,0x6F,0x24,0xC6,0xB8,0x9E,0x4E,0xCD,0xAC,0x24,
			0x35,0x4B,0x9E,0x99,0xCA,0xA3,0xF6,0xD3,0x76,0x14,0x02,0xCD),
		.q = chunk_from_chars(
			0xD7,0xC1,0x34,0xAA,0x26,0x43,0x66,0x86,0x2A,0x18,0x30,0x25,0x75,0xD0,0xFB,0x98,
			0xD1,0x16,0xBC,0x4B,0x6D,0xDE,0xBC,0xA3,0xA5,0xA7,0x93,0x9F),
	},
	{
		/* ECC Brainpool 256-bit curve (RFC 5639), brainpoolP256r1 */
		.group = ECP_256_BP,
		.p = chunk_from_chars(
			0xA9,0xFB,0x57,0xDB,0xA1,0xEE,0xA9,0xBC,0x3E,0x66,0x0A,0x90,0x9D,0x83,0x8D,0x72,
			0x6E,0x3B,0xF6,0x23,0xD5,0x26,0x20,0x28,0x20,0x13,0x48,0x1D,0x1F,0x6E,0x53,0x77),
		.a = chunk_from_chars(
			0x7D,0x5A,0x09,0x75,0xFC,0x2C,0x30,0x57,0xEE,0xF6,0x75,0x30,0x41,0x7A,0xFF,0xE7,
			0xFB,0x80,0x55,0xC1,0x26,0xDC,0x5C,0x6C,0xE9,0x4A,0x4B,0x44,0xF3,0x30,0xB5,0xD9),
		.b = chunk_from_chars(
			0x26,0xDC,0x5C,0x6C,0xE9,0x4A,0x4B,0x44,0xF3,0x30,0xB5,0xD9,0xBB,0xD7,0x7C,0xBF,
			0x95,0x84,0x16,0x29,0x5C,0xF7,0xE1,0xCE,0x6B,0xCC,0xDC,0x18,0xFF,0x8C,0x07,0xB6),
		.x = chunk_from_chars(
			0x8B,0xD2,0xAE,0xB9,0xCB,0x7E,0x57,0xCB,0x2C,0x4B,0x48,0x2F,0xFC,0x81,0xB7,0xAF,
			0xB9,0xDE,0x27,0xE1,0xE3,0xBD,0x23,0xC2,0x3A,0x44,0x53,0xBD,0x9A,0xCE,0x32,0x62),
		.y = chunk_from_chars(
			0x54,0x7E,0xF8,0x35,0xC3,0xDA,0xC4,0xFD,0x97,0xF8,0x46,0x1A,0x14,0x61,0x1D,0xC9,
			0xC2,0x77,0x45,0x13,0x2D,0xED,0x8E,0x54,0x5C,0x1D,0x54,0xC7,0x2F,0x04,0x69,0x97),
		.q = chunk_from_chars(
			0xA9,0xFB,0x57,0xDB,0xA1,0xEE,0xA9,0xBC,0x3E,0x66,0x0A,0x90,0x9D,0x83,0x8D,0x71,
			0x8C,0x39,0x7A,0xA3,0xB5,0x61,0xA6,0xF7,0x90,0x1E,0x0E,0x82,0x97,0x48,0x56,0xA7),
	},
	{
		/* ECC Brainpool 384-bit curve (RFC 5639), brainpoolP384r1 */
		.group = ECP_384_BP,
		.p = chunk_from_chars(
			0x8C,0xB9,0x1E,0x82,0xA3,0x38,0x6D,0x28,0x0F,0x5D,0x6F,0x7E,0x50,0xE6,0x41,0xDF,
			0x15,0x2F,0x71,0x09,0xED,0x54,0x56,0xB4,0x12,0xB1,0xDA,0x19,0x7F,0xB7,0x11,0x23,
			0xAC,0xD3,0xA7,0x29,0x90,0x1D,0x1A,0x71,0x87,0x47,0x00,0x13,0x31,0x07,0xEC,0x53),
		.a = chunk_from_chars(
			0x7B,0xC3,0x82,0xC6,0x3D,0x8C,0x15,0x0C,0x3C,0x72,0x08,0x0A,0xCE,0x05,0xAF,0xA0,
			0xC2,0xBE,0xA2,0x8E,0x4F,0xB2,0x27,0x87,0x13,0x91,0x65,0xEF,0xBA,0x91,0xF9,0x0F,
			0x8A,0xA5,0x81,0x4A,0x50,0x3A,0xD4,0xEB,0x04,0xA8,0xC7,0xDD,0x22,0xCE,0x28,0x26),
		.b = chunk_from_chars(
			0x04,0xA8,0xC7,0xDD,0x22,0xCE,0x28,0x26,0x8B,0x39,0xB5,0x54,0x16,0xF0,0x44,0x7C,
			0x2F,0xB7,0x7D,0xE1,0x07,0xDC,0xD2,0xA6,0x2E,0x88,0x0E,0xA5,0x3E,0xEB,0x62,0xD5,
			0x7C,0xB4,0x39,0x02,0x95,0xDB,0xC9,0x94,0x3A,0xB7,0x86,0x96,0xFA,0x50,0x4C,0x11),
		.x = chunk_from_chars(
			0x1D,0x1C,0x64,0xF0,0x68,0xCF,0x45,0xFF,0xA2,0xA6,0x3A,0x81,0xB7,0xC1,0x3F,0x6B,
			0x88,0x47,0xA3,0xE7,0x7E,0xF1,0x4F,0xE3,0xDB,0x7F,0xCA,0xFE,0x0C,0xBD,0x10,0xE8,
			0xE8,0x26,0xE0,0x34,0x36,0xD6,0x46,0xAA,0xEF,0x87,0xB2,0xE2,0x47,0xD4,0xAF,0x1E),
		.y = chunk_from_chars(
			0x8A,0xBE,0x1D,0x75,0x20,0xF9,0xC2,0xA4,0x5C,0xB1,0xEB,0x8E,0x95,0xCF,0xD5,0x52,
			0x62,0xB7,0x0B,0x29,0xFE,0xEC,0x58,0x64,0xE1,0x9C,0x05,0x4F,0xF9,0x91,0x29,0x28,
			0x0E,0x46,0x46,0x21,0x77,0x91,0x81,0x11,0x42,0x82,0x03,0x41,0x26,0x3C,0x53,0x15),
		.q = chunk_from_chars(
			0x8C,0xB9,0x1E,0x82,0xA3,0x38,0x6D,0x28,0x0F,0x5D,0x6F,0x7E,0x50,0xE6,0x41,0xDF,
			0x15,0x2F,0x71,0x09,0xED,0x54,0x56,0xB3,0x1F,0x16,0x6E,0x6C,0xAC,0x04,0x25,0xA7,
			0xCF,0x3A,0xB6,0xAF,0x6B,0x7F,0xC3,0x10,0x3B,0x88,0x32,0x02,0xE9,0x04,0x65,0x65),
	},
	{
		/* ECC Brainpool 512-bit curve (RFC 5639), brainpoolP512r1 */
		.group = ECP_512_BP,
		.p = chunk_from_chars(
			0xAA,0xDD,0x9D,0xB8,0xDB,0xE9,0xC4,0x8B,0x3F,0xD4,0xE6,0xAE,0x33,0xC9,0xFC,0x07,
			0xCB,0x30,0x8D,0xB3,0xB3,0xC9,0xD2,0x0E,0xD6,0x63,0x9C,0xCA,0x70,0x33,0x08,0x71,
			0x7D,0x4D,0x9B,0x00,0x9B,0xC6,0x68,0x42,0xAE,0xCD,0xA1,0x2A,0xE6,0xA3,0x80,0xE6,
			0x28,0x81,0xFF,0x2F,0x2D,0x82,0xC6,0x85,0x28,0xAA,0x60,0x56,0x58,0x3A,0x48,0xF3),
		.a = chunk_from_chars(
			0x78,0x30,0xA3,0x31,0x8B,0x60,0x3B,0x89,0xE2,0x32,0x71,0x45,0xAC,0x23,0x4C,0xC5,
			0x94,0xCB,0xDD,0x8D,0x3D,0xF9,0x16,0x10,0xA8,0x34,0x41,0xCA,0xEA,0x98,0x63,0xBC,
			0x2D,0xED,0x5D,0x5A,0xA8,0x25,0x3A,0xA1,0x0A,0x2E,0xF1,0xC9,0x8B,0x9A,0xC8,0xB5,
			0x7F,0x11,0x17,0xA7,0x2B,0xF2,0xC7,0xB9,0xE7,0xC1,0xAC,0x4D,0x77,0xFC,0x94,0xCA),
		.b = chunk_from_chars(
			0x3D,0xF9,0x16,0x10,0xA8,0x34,0x41,0xCA,0xEA,0x98,0x63,0xBC,0x2D,0xED,0x5D,0x5A,
			0xA8,0x25,0x3A,0xA1,0x0A,0x2E,0xF1,0xC9,0x8B,0x9A,0xC8,0xB5,0x7F,0x11,0x17,0xA7,
			0x2B,0xF2,0xC7,0xB9,0xE7,0xC1,0xAC,0x4D,0x77,0xFC,0x94,0xCA,0xDC,0x08,0x3E,0x67,
			0x98,0x40,0x50,0xB7,0x5E,0xBA,0xE5,0xDD,0x28,0x09,0xBD,0x63,0x80,0x16,0xF7,0x23),
		.x = chunk_from_chars(
			0x81,0xAE,0xE4,0xBD,0xD8,0x2E,0xD9,0x64,0x5A,0x21,0x32,0x2E,0x9C,0x4C,0x6A,0x93,
			0x85,0xED,0x9F,0x70,0xB5,0xD9,0x16,0xC1,0xB4,0x3B,0x62,0xEE,0xF4,0xD0,0x09,0x8E,
			0xFF,0x3B,0x1F,0x78,0xE2,0xD0,0xD4,0x8D,0x50,0xD1,0x68,0x7B,0x93,0xB9,0x7D,0x5F,
			0x7C,0x6D,0x50,0x47,0x40,0x6A,0x5E,0x68,0x8B,0x35,0x22,0x09,0xBC,0xB9,0xF8,0x22),
		.y = chunk_from_chars(
			0x7D,0xDE,0x38,0x5D,0x56,0x63,0x32,0xEC,0xC0,0xEA,0xBF,0xA9,0xCF,0x78,0x22,0xFD,
			0xF2,0x09,0xF7,0x00,0x24,0xA5,0x7B,0x1A,0xA0,0x00,0xC5,0x5B,0x88,0x1F,0x81,0x11,
			0xB2,0xDC,0xDE,0x49,0x4A,0x5F,0x48,0x5E,0x5B,0xCA,0x4B,0xD8,0x8A,0x27,0x63,0xAE,
			0xD1,0xCA,0x2B,0x2F,0xA8,0xF0,0x54,0x06,0x78,0xCD,0x1E,0x0F,0x3A,0xD8,0x08,0x92),
		.q = chunk_from_chars(
			0xAA,0xDD,0x9D,0xB8,0xDB,0xE9,0xC4,0x8B,0x3F,0xD4,0xE6,0xAE,0x33,0xC9,0xFC,0x07,
			0xCB,0x30,0x8D,0xB3,0xB3,0xC9,0xD2,0x0E,0xD6,0x63,0x9C,0xCA,0x70,0x33,0x08,0x70,
			0x55,0x3E,0x5C,0x41,0x4C,0xA9,0x26,0x19,0x41,0x86,0x61,0x19,0x7F,0xAC,0x10,0x47,
			0x1D,0xB1,0xD3,0x81,0x08,0x5D,0xDA,0xDD,0xB5,0x87,0x96,0x82,0x9C,0xA9,0x00,0x69),
	},
};

/**
 * Create an EC_GROUP object for an ECC Brainpool curve
 */
EC_GROUP *ec_group_new_brainpool(bp_curve *curve)
{
	BIGNUM *p, *a, *b, *x, *y, *q;
	const BIGNUM *h;
	EC_POINT *G = NULL;
	EC_GROUP *group = NULL, *result = NULL;
	BN_CTX *ctx = NULL;

	ctx = BN_CTX_new();
	p = BN_bin2bn(curve->p.ptr, curve->p.len, NULL);
	a = BN_bin2bn(curve->a.ptr, curve->a.len, NULL);
	b = BN_bin2bn(curve->b.ptr, curve->b.len, NULL);
	x = BN_bin2bn(curve->x.ptr, curve->x.len, NULL);
	y = BN_bin2bn(curve->y.ptr, curve->y.len, NULL);
	q = BN_bin2bn(curve->q.ptr, curve->q.len, NULL);
	/* all supported groups have a cofactor of 1 */
	h = BN_value_one();
	if (!ctx || !p || !a || !b || !x || !y || !q)
	{
		goto failed;
	}
	group = EC_GROUP_new_curve_GFp(p, a, b, ctx);
	if (!group)
	{
		goto failed;
	}
	G = EC_POINT_new(group);
	if (!G || !EC_POINT_set_affine_coordinates_GFp(group, G, x, y, ctx))
	{
		goto failed;
	}
	if (!EC_GROUP_set_generator(group, G, q, h))
	{
		goto failed;
	}
	result = group;

failed:
	if (!result && group)
	{
		EC_GROUP_free(group);
	}
	if (G)
	{
		EC_POINT_free(G);
	}
	BN_CTX_free(ctx);
	BN_free(p);
	BN_free(a);
	BN_free(b);
	BN_free(x);
	BN_free(y);
	BN_free(q);
	return result;
}

/**
 * Create an EC_KEY for ECC Brainpool curves as defined above
 */
static EC_KEY *ec_key_new_brainpool(diffie_hellman_group_t group)
{
	bp_curve *curve = NULL;
	EC_GROUP *ec_group;
	EC_KEY *key = NULL;
	int i;

	for (i = 0; i < countof(bp_curves); i++)
	{
		if (bp_curves[i].group == group)
		{
			curve = &bp_curves[i];
		}
	}
	if (!curve)
	{
		return NULL;
	}
	ec_group = ec_group_new_brainpool(curve);
	if (!ec_group)
	{
		return NULL;
	}
	key = EC_KEY_new();
	if (!key || !EC_KEY_set_group(key, ec_group))
	{
		EC_KEY_free(key);
		key = NULL;
	}
	EC_GROUP_free(ec_group);
	return key;
}

#else /* !NID_brainpoolP224r1 || ... */

/**
 * Create an EC_KEY for ECC Brainpool curves as defined by OpenSSL
 */
static EC_KEY *ec_key_new_brainpool(diffie_hellman_group_t group)
{
	switch (group)
	{
		case ECP_224_BP:
			return EC_KEY_new_by_curve_name(NID_brainpoolP224r1);
		case ECP_256_BP:
			return EC_KEY_new_by_curve_name(NID_brainpoolP256r1);
		case ECP_384_BP:
			return EC_KEY_new_by_curve_name(NID_brainpoolP384r1);
		case ECP_512_BP:
			return EC_KEY_new_by_curve_name(NID_brainpoolP512r1);
		default:
			return NULL;
	}
}

#endif /* !NID_brainpoolP224r1 || ... */

/*
 * Described in header.
 */
openssl_ec_diffie_hellman_t *openssl_ec_diffie_hellman_create(diffie_hellman_group_t group)
{
	private_openssl_ec_diffie_hellman_t *this;

	INIT(this,
		.public = {
			.dh = {
				.get_shared_secret = _get_shared_secret,
				.set_other_public_value = _set_other_public_value,
				.get_my_public_value = _get_my_public_value,
				.set_private_value = _set_private_value,
				.get_dh_group = _get_dh_group,
				.destroy = _destroy,
			},
		},
		.group = group,
	);

	switch (group)
	{
		case ECP_192_BIT:
			this->key = EC_KEY_new_by_curve_name(NID_X9_62_prime192v1);
			break;
		case ECP_224_BIT:
			this->key = EC_KEY_new_by_curve_name(NID_secp224r1);
			break;
		case ECP_256_BIT:
			this->key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
			break;
		case ECP_384_BIT:
			this->key = EC_KEY_new_by_curve_name(NID_secp384r1);
			break;
		case ECP_521_BIT:
			this->key = EC_KEY_new_by_curve_name(NID_secp521r1);
			break;
		case ECP_224_BP:
		case ECP_256_BP:
		case ECP_384_BP:
		case ECP_512_BP:
			this->key = ec_key_new_brainpool(group);
			break;
		default:
			this->key = NULL;
			break;
	}

	if (!this->key)
	{
		free(this);
		return NULL;
	}

	/* caching the EC group */
	this->ec_group = EC_KEY_get0_group(this->key);

	this->pub_key = EC_POINT_new(this->ec_group);
	if (!this->pub_key)
	{
		destroy(this);
		return NULL;
	}

	/* generate an EC private (public) key */
	if (!EC_KEY_generate_key(this->key))
	{
		destroy(this);
		return NULL;
	}

	return &this->public;
}
#endif /* OPENSSL_NO_EC */
