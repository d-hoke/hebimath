/*
 * hebimath - arbitrary precision arithmetic library
 * See LICENSE file for copyright and license details
 */

#include "../../internal.h"

HEBI_API
size_t
hebi_zgetstr(
		char *restrict str,
		size_t len,
		hebi_zsrcptr restrict a,
		unsigned int base,
		unsigned int flags )
{
	char *ptr;              /* pointer to after sign character in output */
	char *end;              /* pointer to null-terminator of output */
	size_t rlen;            /* result length of string */
	size_t slen;            /* capacity of str after writing sign */
	hebi_packet *w;         /* scratchpad copy of 'a' packets */
	size_t n;               /* number of packets in 'a' */
	int s;                  /* sign flag of 'a' */

	/* validate input arguments */
	if (UNLIKELY(base < 2 || 64 < base))
		hebi_error_raise(HEBI_ERRDOM_HEBI, HEBI_EBADVALUE);

	/* setup pointers and result length */
	ptr = str;
	end = str + len - (len > 0);
	rlen = 0;
	slen = len;

	/* write out negative sign or optional plus sign */
	s = a->hz_sign;
	if (s < 0) {
		rlen++;
		if (LIKELY(ptr < end)) {
			*ptr = '-';
			ptr++;
			slen--;
		}
	} else if (flags & HEBI_STR_SIGN) {
		rlen++;
		if (LIKELY(ptr < end)) {
			*ptr = '+';
			ptr++;
			slen--;
		}
	}

	/* copy integer data into scratch-pad buffer if non-zero */
	if (LIKELY(s)) {
		n = a->hz_used;
		w = hebi_pscratch__(n);
		hebi_pcopy(w, a->hz_packs, n);
	} else {
		n = 0;
		w = NULL;
	}

	/* determine string from packet sequence */
	return rlen + hebi_pgetstr(ptr, slen, w, n, base, flags);
}
