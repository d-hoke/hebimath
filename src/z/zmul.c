/*
 * hebimath - arbitrary precision arithmetic library
 * See LICENSE file for copyright and license details
 */

#include "../../internal.h"

HEBI_API
void
hebi_zmul(hebi_zptr r, hebi_zsrcptr a, hebi_zsrcptr b)
{
	hebi_z rtmp;
	hebi_zptr restrict rz;
	hebi_packet *restrict rp, *restrict wp;
	size_t rn, an, bn;
	int as, bs;

	if (UNLIKELY(!(as = a->hz_sign) || !(bs = b->hz_sign))) {
		hebi_zsetzero(r);
		return;
	}

	an = a->hz_used;
	bn = b->hz_used;
	if (an < bn) {
		SWAP(hebi_zsrcptr, a, b);
		SWAP(size_t, an, bn);
	}

	rn = an + bn + 1;
	if (UNLIKELY(rn <= an))
		hebi_error_raise(HEBI_ERRDOM_HEBI, HEBI_EBADLENGTH);

	rz = r;
	if (rz == a || rz == b)
		hebi_zinit_push__((rz = rtmp), hebi_zallocator(r));

	if (an > KARATSUBA_MUL_CUTOFF) {
		wp = hebi_pscratch__(hebi_pmul_karatsuba_space(an, bn));
		rp = hebi_zgrow__(rz, rn);
		hebi_pzero(rp, rn);
		hebi_pmul_karatsuba(rp, wp, a->hz_packs, b->hz_packs, an, bn);
	} else {
		rp = hebi_zgrow__(rz, --rn);
		hebi_pmul(rp, a->hz_packs, b->hz_packs, an, bn);
	}

	rz->hz_used = hebi_pnorm(rp, rn);
	rz->hz_sign = (as ^ bs) < 0 ? -1 : 1;

	if (rz != r) {
		hebi_zswap(rz, r);
		hebi_zdestroy_pop__(rz);
	}
}
