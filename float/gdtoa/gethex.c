/****************************************************************

The author of this software is David M. Gay.

Copyright (C) 1998 by Lucent Technologies
All Rights Reserved

Permission to use, copy, modify, and distribute this software and
its documentation for any purpose and without fee is hereby
granted, provided that the above copyright notice appear in all
copies and that both that the copyright notice and this
permission notice and warranty disclaimer appear in supporting
documentation, and that the name of Lucent or any of its entities
not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior
permission.

LUCENT DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.
IN NO EVENT SHALL LUCENT OR ANY OF ITS ENTITIES BE LIABLE FOR ANY
SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.

****************************************************************/

/* Please send bug reports to
	David M. Gay
	Bell Laboratories, Room 2C-463
	600 Mountain Avenue
	Murray Hill, NJ 07974-0636
	U.S.A.
	dmg@bell-labs.com
 */

#include "gdtoaimp.h"

 int
#ifdef KR_headers
gethex(sp, fpi, exp, bp, sign)
	CONST char **sp; FPI *fpi; Long *exp; Bigint **bp; int sign;
#else
gethex( CONST char **sp, FPI *fpi, Long *exp, Bigint **bp, int sign)
#endif
{
	static unsigned char hexdig[256];	/* assume 8-bit bytes */
	Bigint *b;
	CONST unsigned char *decpt, *s0, *s, *s1;
	int esign, havedig, irv, k, n, nbits, up;
	ULong L, lostbits, *x;
	Long e, e1;

	if (!hexdig['0'])
		hexdig_init_D2A();
	havedig = 0;
	s0 = *(CONST unsigned char **)sp + 2;
	while(s0[havedig] == '0')
		havedig++;
	s0 += havedig;
	s = s0;
	decpt = 0;
	if (!hexdig[*s]) {
		if (*s == '.') {
			decpt = ++s;
			if (!hexdig[*s])
				goto ret0;
			}
		else {
 ret0:
			*sp = (char*)s;
			return havedig ? STRTOG_Zero : STRTOG_NoNumber;
			}
		while(*s == '0')
			s++;
		havedig = 1;
		if (!hexdig[*s])
			goto ret0;
		s0 = s;
		}
	while(hexdig[*s])
		s++;
	if (*s == '.' && !decpt) {
		decpt = ++s;
		while(hexdig[*s])
			s++;
		}
	e = 0;
	if (decpt)
		e = -(((Long)(s-decpt)) << 2);
	s1 = s;
	switch(*s) {
	  case 'p':
	  case 'P':
		esign = 0;
		switch(*++s) {
		  case '-':
			esign = 1;
			/* no break */
		  case '+':
			s++;
		  }
		if ((n = hexdig[*s]) == 0 || n > 0x19) {
			s = s1;
			break;
			}
		e1 = n - 0x10;
		while((n = hexdig[*++s]) !=0 && n <= 0x19)
			e1 = 10*e1 + n - 0x10;
		if (esign)
			e1 = -e1;
		e += e1;
	  }
	*sp = (char*)s;
	n = s1 - s0 - 1;
	for(k = 0; n > 7; n >>= 1)
		k++;
	b = Balloc(k);
	x = b->x;
	n = 0;
	L = 0;
	while(s1 > s0) {
		if (*--s1 == '.')
			continue;
		if (n == 32) {
			*x++ = L;
			L = 0;
			n = 0;
			}
		L |= (hexdig[*s1] & 0x0f) << n;
		n += 4;
		}
	*x++ = L;
	b->wds = n = x - b->x;
	n = 32*n - hi0bits(L);
	nbits = fpi->nbits;
	lostbits = 0;
	x = b->x;
	if (n > nbits) {
		n -= nbits;
		if (any_on(b,n)) {
			lostbits = 1;
			k = n - 1;
			if (x[k>>kshift] & 1 << (k & kmask)) {
				lostbits = 2;
				if (k > 1 && any_on(b,k-1))
					lostbits = 3;
				}
			}
		rshift(b, n);
		e += n;
		}
	else if (n < nbits) {
		n = nbits - n;
		b = lshift(b, n);
		e -= n;
		x = b->x;
		}
	if (e > fpi->emax) {
 ovfl:
		Bfree(b);
		return STRTOG_Infinite | STRTOG_Overflow | STRTOG_Inexhi;
		}
	irv = STRTOG_Normal;
	if (e < fpi->emin) {
		irv = STRTOG_Denormal;
		n = fpi->emin - e;
		if (n >= nbits) {
			switch (fpi->rounding) {
			  case FPI_Round_near:
				if (n == nbits && n < 2 || any_on(b,n-1))
					goto one_bit;
				break;
			  case FPI_Round_up:
				if (!sign)
					goto one_bit;
				break;
			  case FPI_Round_down:
				if (sign) {
 one_bit:
					*exp = fpi->emin;
					x[0] = b->wds = 1;
					*bp = b;
					return STRTOG_Denormal | STRTOG_Inexhi
						| STRTOG_Underflow;
					}
			  }
			Bfree(b);
			return STRTOG_Zero | STRTOG_Inexlo | STRTOG_Underflow;
			}
		k = n - 1;
		if (lostbits)
			lostbits = 1;
		else if (k > 0)
			lostbits = any_on(b,k);
		if (x[k>>kshift] & 1 << (k & kmask))
			lostbits |= 2;
		nbits -= n;
		rshift(b,n);
		e = fpi->emin;
		}
	if (lostbits) {
		up = 0;
		switch(fpi->rounding) {
		  case FPI_Round_zero:
			break;
		  case FPI_Round_near:
			if (lostbits & 2
			 && (lostbits & 1) | x[0] & 1)
				up = 1;
			break;
		  case FPI_Round_up:
			up = 1 - sign;
			break;
		  case FPI_Round_down:
			up = sign;
		  }
		if (up) {
			k = b->wds;
			b = increment(b);
			x = b->x;
			if (b->wds > k
			 || (n = nbits & kmask) !=0
			     && hi0bits(x[k-1]) < 32-n) {
				rshift(b,1);
				if (++e > fpi->emax)
					goto ovfl;
				}
			else if (irv == STRTOG_Denormal) {
				k = nbits - 1;
				if (x[k >> kshift] & 1 << (k & kmask))
					irv = STRTOG_Normal;
				}
			irv |= STRTOG_Inexhi;
			}
		else
			irv |= STRTOG_Inexlo;
		}
	*bp = b;
	*exp = e;
	return irv;
	}
