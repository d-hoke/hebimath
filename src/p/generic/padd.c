/*
 * hebimath - arbitrary precision arithmetic library
 * See LICENSE file for copyright and license details
 */

#include "generic.h"

HEBI_API
uint64_t
hebi_padd(
		hebi_packet *r,
		const hebi_packet *a,
		const hebi_packet *b,
		size_t an,
		size_t bn )
{
	LIMB *rl;
	const LIMB *al;
	const LIMB *bl;
	LIMB sum, carry;
	size_t i, m, n;

	ASSERT(an >= bn);
	ASSERT(bn > 0);

	rl = LIMB_PTR(r);
	al = LIMB_PTR(a);
	bl = LIMB_PTR(b);
	m = an * LIMB_PER_PACKET;
	n = bn * LIMB_PER_PACKET;

	carry = 0;
	i = 0;

	do {
		sum = al[i] + bl[i] + carry;
		carry = (sum < al[i]) || (sum == al[i] && carry);
		rl[i] = sum;
	} while (++i < n);

	for ( ; i < m; i++) {
		sum = al[i] + carry;
		carry = sum < al[i];
		rl[i] = sum;
	}

	return carry;
}
