/*
 *      CVSD (Continuously Variable Slope Delta modulation)
 *      conversion routines
 *
 *      The CVSD format is described in the MIL Std 188 113, which is
 *      available from http://bbs.itsi.disa.mil:5580/T3564
 *
 *	Copyright (C) 1996  
 *      Thomas Sailer (sailer@ife.ee.ethz.ch) (HB9JNX/AE4WA)
 *      Swiss Federal Institute of Technology, Electronics Lab
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Change History:
 *
 * June 1, 1998 - Chris Bagwell (cbagwell@sprynet.com)
 *   Fixed compile warnings reported by Kjetil Torgrim Homme
 *   <kjetilho@ifi.uio.no>
 *
 *
 */

/* ---------------------------------------------------------------------- */

#include <limits.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>	/* For SEEK_* defines if not found in stdio */
#endif

#include "cvsdfilt.h"
#include "st.h"

/* ---------------------------------------------------------------------- */

#ifndef HAVE_MEMMOVE
#define memmove(dest,src,len) (bcopy((src),(dest),(len)))
#endif

/* ---------------------------------------------------------------------- */
/*
 * private data structures
 */

struct cvsd_common_state {
	unsigned overload;
	float mla_int;
	float mla_tc0;
	float mla_tc1;
	unsigned phase;
	unsigned phase_inc;
	float v_min, v_max;
};

struct cvsd_decode_state {
	float output_filter[DEC_FILTERLEN];
};

struct cvsd_encode_state {
	float recon_int;
	float input_filter[ENC_FILTERLEN];
};

struct cvsdpriv {
	struct cvsd_common_state com;
	union {
		struct cvsd_decode_state dec;
		struct cvsd_encode_state enc;
	} c;
	struct {
		unsigned shreg;
		unsigned mask;
		unsigned cnt;
	} bit;
	unsigned bytes_written;
	unsigned cvsd_rate;
	char swapbits;
};

/* ---------------------------------------------------------------------- */

static float float_conv(fp1, fp2, n)
float *fp1;
float *fp2;
int n;
{
	float res = 0;
	for(; n > 0; n--)
		res += (*fp1++) * (*fp2++);
	return res;
}

/* ---------------------------------------------------------------------- */
/*
 * some remarks about the implementation of the CVSD decoder
 * the principal integrator is integrated into the output filter
 * to achieve this, the coefficients of the output filter are multiplied
 * with (1/(1-1/z)) in the initialisation code.
 * the output filter must have a sharp zero at f=0 (i.e. the sum of the
 * filter parameters must be zero). This prevents an accumulation of
 * DC voltage at the principal integration.
 */
/* ---------------------------------------------------------------------- */

static void cvsdstartcommon(ft)
ft_t ft;
{
	struct cvsdpriv *p = (struct cvsdpriv *) ft->priv;
	
	/* sanity check */
	if (sizeof(struct cvsdpriv) > PRIVSIZE)
		fail("struct cvsdpriv is too big (%d); change PRIVSIZE in st.h and recompile sox", sizeof(struct cvsdpriv));
	p->cvsd_rate = (ft->info.rate <= 24000) ? 16000 : 32000;
	ft->info.rate = 8000;
	ft->info.channels = 1;
	ft->info.size = WORD; /* make output format default to words */
	ft->info.style = SIGN2;
	p->swapbits = ft->swap;
	ft->swap = 0;
	/*
	 * initialize the decoder
	 */
	p->com.overload = 0x5;
	p->com.mla_int = 0;
	/*
	 * timeconst = (1/e)^(200 / SR) = exp(-200/SR)
	 * SR is the sampling rate
	 */
	p->com.mla_tc0 = exp((-200.0)/((float)(p->cvsd_rate)));
	/*
	 * phase_inc = 32000 / SR
	 */
	p->com.phase_inc = 32000 / p->cvsd_rate;
	/*
	 * initialize bit shift register
	 */
	p->bit.shreg = p->bit.cnt = 0;
	p->bit.mask = p->swapbits ? 0x80 : 1;
	/*
	 * count the bytes written
	 */
	p->bytes_written = 0;
	p->com.v_min = 1;
	p->com.v_max = -1;
	report("cvsd: bit rate %dbit/s, bits from %s\n", p->cvsd_rate,
	       p->swapbits ? "msb to lsb" : "lsb to msb");
}

/* ---------------------------------------------------------------------- */

void cvsdstartread(ft) 
ft_t ft;
{
	struct cvsdpriv *p = (struct cvsdpriv *) ft->priv;
	float *fp1;
	int i;
	
	cvsdstartcommon(ft);
	p->com.mla_tc1 = 0.1 * (1 - p->com.mla_tc0);
	p->com.phase = 0;
	/*
	 * initialize the output filter coeffs (i.e. multiply
	 * the coeffs with (1/(1-1/z)) to achieve integration
	 * this is now done in the filter parameter generation utility
	 */
	/*
	 * zero the filter 
	 */
	for(fp1 = p->c.dec.output_filter, i = DEC_FILTERLEN; i > 0; i--)
		*fp1++ = 0;
}

/* ---------------------------------------------------------------------- */

void cvsdstartwrite(ft) 
ft_t ft;
{
	struct cvsdpriv *p = (struct cvsdpriv *) ft->priv;
	float *fp1;
	int i;

	cvsdstartcommon(ft);
	p->com.mla_tc1 = 0.1 * (1 - p->com.mla_tc0);
	p->com.phase = 4;
	/*
	 * zero the filter 
	 */
	for(fp1 = p->c.enc.input_filter, i = ENC_FILTERLEN; i > 0; i--)
		*fp1++ = 0;
	p->c.enc.recon_int = 0;
}

/* ---------------------------------------------------------------------- */

void
cvsdstopwrite(ft)
ft_t ft;
{
	struct cvsdpriv *p = (struct cvsdpriv *) ft->priv;

	if (p->bit.cnt) {
		putc(p->bit.shreg, ft->fp);
		p->bytes_written++;
	}
	report("cvsd: min slope %f, max slope %f\n", 
	       p->com.v_min, p->com.v_max);	
}

/* ---------------------------------------------------------------------- */

void
cvsdstopread(ft)
ft_t ft;
{
	struct cvsdpriv *p = (struct cvsdpriv *) ft->priv;

	report("cvsd: min value %f, max value %f\n", 
	       p->com.v_min, p->com.v_max);
}

/* ---------------------------------------------------------------------- */

#undef DEBUG

#ifdef DEBUG
static struct {
	FILE *f1;
	FILE *f2;
	int cnt
} dbg = { NULL, NULL, 0 };
#endif

LONG cvsdread(ft, buf, nsamp) 
ft_t ft;
LONG *buf, nsamp;
{
	struct cvsdpriv *p = (struct cvsdpriv *) ft->priv;
	int done = 0;
	float oval;
	
#ifdef DEBUG
	if (!dbg.f1) {
		if (!(dbg.f1 = fopen("dbg1", "w")))
			fail("debugging");
		fprintf(dbg.f1, "\"input\"\n");
	}
	if (!dbg.f2) {
		if (!(dbg.f2 = fopen("dbg2", "w")))
			fail("debugging");
		fprintf(dbg.f2, "\"recon\"\n");
	}
#endif
	while (done < nsamp) {
		if (!p->bit.cnt) {
			p->bit.shreg = getc(ft->fp);
			if (feof(ft->fp))
				return done;
			p->bit.cnt = 8;
			p->bit.mask = p->swapbits ? 0x80 : 1;
		}
		/*
		 * handle one bit
		 */
		p->bit.cnt--;
		p->com.overload = ((p->com.overload << 1) | 
				   (!!(p->bit.shreg & p->bit.mask))) & 7;
		if (p->swapbits)
			p->bit.mask >>= 1;
		else
			p->bit.mask <<= 1;
		p->com.mla_int *= p->com.mla_tc0;
		if ((p->com.overload == 0) || (p->com.overload == 7))
			p->com.mla_int += p->com.mla_tc1;
		memmove(p->c.dec.output_filter+1, p->c.dec.output_filter,
			sizeof(p->c.dec.output_filter)-sizeof(float));
		if (p->com.overload & 1)
			p->c.dec.output_filter[0] = p->com.mla_int;
		else
			p->c.dec.output_filter[0] = -p->com.mla_int;
		/*
		 * check if the next output is due
		 */
		p->com.phase += p->com.phase_inc;
		if (p->com.phase >= 4) {
			oval = float_conv(p->c.dec.output_filter, 
					  (p->cvsd_rate < 24000) ? 
					  dec_filter_16 : dec_filter_32, 
					  DEC_FILTERLEN);
#ifdef DEBUG
			fprintf(dbg.f1, "%f %f\n", (double)dbg.cnt, 
				(double)p->com.mla_int);
			fprintf(dbg.f2, "%f %f\n", (double)dbg.cnt, 
				(double)oval);
			dbg.cnt++;
#endif		
			if (oval > p->com.v_max)
				p->com.v_max = oval;
			if (oval < p->com.v_min)
				p->com.v_min = oval;
			*buf++ = (oval * ((float)LONG_MAX));
			done++;
		}
		p->com.phase &= 3;
	}
	return done;
}

/* ---------------------------------------------------------------------- */

void
cvsdwrite(ft, buf, nsamp) 
ft_t ft;
LONG *buf, nsamp;
{
	struct cvsdpriv *p = (struct cvsdpriv *) ft->priv;
	int done = 0;
	float inval;

#ifdef DEBUG
	if (!dbg.f1) {
		if (!(dbg.f1 = fopen("dbg1", "w")))
			fail("debugging");
		fprintf(dbg.f1, "\"input\"\n");
	}
	if (!dbg.f2) {
		if (!(dbg.f2 = fopen("dbg2", "w")))
			fail("debugging");
		fprintf(dbg.f2, "\"recon\"\n");
	}
#endif
	for(;;) {
		/*
		 * check if the next input is due
		 */
		if (p->com.phase >= 4) {
			if (done >= nsamp)
				return;
			memmove(p->c.enc.input_filter+1, p->c.enc.input_filter,
				sizeof(p->c.enc.input_filter)-sizeof(float));
			p->c.enc.input_filter[0] = (*buf++) / 
				((float)LONG_MAX);
			done++;
		}
		p->com.phase &= 3;
		/* insert input filter here! */
		inval = float_conv(p->c.enc.input_filter, 
				   (p->cvsd_rate < 24000) ? 
				   (enc_filter_16[(p->com.phase >= 2)]) : 
				   (enc_filter_32[p->com.phase]), 
				   ENC_FILTERLEN);
		/*
		 * encode one bit
		 */
		p->com.overload = (((p->com.overload << 1) |
				    (inval >  p->c.enc.recon_int)) & 7);
		p->com.mla_int *= p->com.mla_tc0;
		if ((p->com.overload == 0) || (p->com.overload == 7))
			p->com.mla_int += p->com.mla_tc1;
		if (p->com.mla_int > p->com.v_max)
			p->com.v_max = p->com.mla_int;
		if (p->com.mla_int < p->com.v_min)
			p->com.v_min = p->com.mla_int;
		if (p->com.overload & 1) {
			p->c.enc.recon_int += p->com.mla_int;
			p->bit.shreg |= p->bit.mask;
		} else
			p->c.enc.recon_int -= p->com.mla_int;
		if ((++(p->bit.cnt)) >= 8) {
			putc(p->bit.shreg, ft->fp);
			p->bytes_written++;
			p->bit.shreg = p->bit.cnt = 0;
			p->bit.mask = p->swapbits ? 0x80 : 1;
		} else {
			if (p->swapbits)
				p->bit.mask >>= 1;
			else
				p->bit.mask <<= 1;
		}
		p->com.phase += p->com.phase_inc;
#ifdef DEBUG
		fprintf(dbg.f1, "%f %f\n", (double)dbg.cnt, (double)inval);
		fprintf(dbg.f2, "%f %f\n", (double)dbg.cnt, 
			(double)p->c.enc.recon_int);
		dbg.cnt++;
#endif	
	}
}

/* ---------------------------------------------------------------------- */
/*
 * DVMS file header
 */
struct dvms_header {
	char          Filename[14];
	unsigned      Id;
	unsigned      State;
	time_t        Unixtime;
	unsigned      Usender;
	unsigned      Ureceiver;
	ULONG	      Length;
	unsigned      Srate;
	unsigned      Days;
	unsigned      Custom1;
	unsigned      Custom2;
	char          Info[16];
	char          extend[64];
	unsigned      Crc;
};

#define DVMS_HEADER_LEN 120

/* ---------------------------------------------------------------------- */
/* SJB: should these be in misc.c instead? */
static ULONG get32(p)
unsigned char **p;
{
	ULONG val = (((*p)[3]) << 24) | (((*p)[2]) << 16) | 
		(((*p)[1]) << 8) | (**p);
	(*p) += 4;
	return val;
}

static unsigned get16(p)
unsigned char **p;
{
	unsigned val = (((*p)[1]) << 8) | (**p);
	(*p) += 2;
	return val;
}

static void put32(p, val)
unsigned char **p;
ULONG val;
{
	*(*p)++ = val & 0xff;
	*(*p)++ = (val >> 8) & 0xff;
	*(*p)++ = (val >> 16) & 0xff;
	*(*p)++ = (val >> 24) & 0xff;
}

static void put16(p, val)
unsigned char **p;
unsigned val;
{
	*(*p)++ = val & 0xff;
	*(*p)++ = (val >> 8) & 0xff;
}

/* ---------------------------------------------------------------------- */

static void dvms_read_header(f, hdr)
FILE *f;
struct dvms_header *hdr;
{
	unsigned char hdrbuf[DVMS_HEADER_LEN];
	unsigned char *pch = hdrbuf;
	int i;
	unsigned sum;

	if (fread(hdrbuf, sizeof(hdrbuf), 1, f) != 1)
		fail("unable to read DVMS header\n");
	for(i = sizeof(hdrbuf), sum = 0; i > /*2*/3; i--) /* Deti bug */
		sum += *pch++;
	pch = hdrbuf;
	memcpy(hdr->Filename, pch, sizeof(hdr->Filename));
	pch += sizeof(hdr->Filename);
	hdr->Id = get16(&pch);
	hdr->State = get16(&pch);
	hdr->Unixtime = get32(&pch);
	hdr->Usender = get16(&pch);
	hdr->Ureceiver = get16(&pch);
	hdr->Length = get32(&pch);
	hdr->Srate = get16(&pch);
	hdr->Days = get16(&pch);
	hdr->Custom1 = get16(&pch);
	hdr->Custom2 = get16(&pch);
	memcpy(hdr->Info, pch, sizeof(hdr->Info));
	pch += sizeof(hdr->Info);
	memcpy(hdr->extend, pch, sizeof(hdr->extend));
	pch += sizeof(hdr->extend);
	hdr->Crc = get16(&pch);
	if (sum != hdr->Crc) 
		fail("DVMS header checksum error, read %u, calculated %u\n",
		     hdr->Crc, sum);
}

/* ---------------------------------------------------------------------- */

/*
 * note! file must be seekable
 */
static void dvms_write_header(f, hdr)
FILE *f;
struct dvms_header *hdr;
{
	unsigned char hdrbuf[DVMS_HEADER_LEN];
	unsigned char *pch = hdrbuf;
	unsigned char *pchs = hdrbuf;
	int i;
	unsigned sum;

	memcpy(pch, hdr->Filename, sizeof(hdr->Filename));
	pch += sizeof(hdr->Filename);
	put16(&pch, hdr->Id);
	put16(&pch, hdr->State);
	put32(&pch, hdr->Unixtime);
	put16(&pch, hdr->Usender);
	put16(&pch, hdr->Ureceiver);
	put32(&pch, hdr->Length);
	put16(&pch, hdr->Srate);
	put16(&pch, hdr->Days);
	put16(&pch, hdr->Custom1);
	put16(&pch, hdr->Custom2);
	memcpy(pch, hdr->Info, sizeof(hdr->Info));
	pch += sizeof(hdr->Info);
	memcpy(pch, hdr->extend, sizeof(hdr->extend));
	pch += sizeof(hdr->extend);
	for(i = sizeof(hdrbuf), sum = 0; i > /*2*/3; i--) /* Deti bug */
		sum += *pchs++;
	hdr->Crc = sum;
	put16(&pch, hdr->Crc);
	if (fseek(f, 0, SEEK_SET) < 0)
		fail("cannot write DVMS header, seek failed\n");
	if (fwrite(hdrbuf, sizeof(hdrbuf), 1, f) != 1)
		fail("cannot write DVMS header\n");
}

/* ---------------------------------------------------------------------- */

static void make_dvms_hdr(ft, hdr)
ft_t ft;
struct dvms_header *hdr;
{
	struct cvsdpriv *p = (struct cvsdpriv *) ft->priv;
	int len;

	memset(hdr->Filename, 0, sizeof(hdr->Filename));
	len = strlen(ft->filename);
	if (len >= sizeof(hdr->Filename))
		len = sizeof(hdr->Filename)-1;
	memcpy(hdr->Filename, ft->filename, len);
	hdr->Id = hdr->State = 0;
	hdr->Unixtime = time(NULL);
	hdr->Usender = hdr->Ureceiver = 0;
	hdr->Length = p->bytes_written;
	hdr->Srate = p->cvsd_rate/100;
	hdr->Days = hdr->Custom1 = hdr->Custom2 = 0;
	memset(hdr->Info, 0, sizeof(hdr->Info));
	len = strlen(ft->comment);
	if (len >= sizeof(hdr->Info))
		len = sizeof(hdr->Info)-1;
	memcpy(hdr->Info, ft->comment, len);
	memset(hdr->extend, 0, sizeof(hdr->extend));
}

/* ---------------------------------------------------------------------- */

void dvmsstartread(ft) 
ft_t ft;
{
	struct cvsdpriv *p = (struct cvsdpriv *) ft->priv;
	struct dvms_header hdr;

	dvms_read_header(ft->fp, &hdr);
	report("DVMS header of source file \"%s\":");
	report("  filename  \"%.14s\"",ft->filename);
        report("  id        0x%x", hdr.Filename);
	report("  state     0x%x", hdr.Id, hdr.State);
	report("  time      %s",ctime(&hdr.Unixtime)); /* ctime generates lf */
	report("  usender   %u", hdr.Usender);
	report("  ureceiver %u", hdr.Ureceiver);
	report("  length    %u", hdr.Length);
	report("  srate     %u", hdr.Srate);
	report("  days      %u", hdr.Days);
	report("  custom1   %u", hdr.Custom1);
	report("  custom2   %u", hdr.Custom2);
	report("  info      \"%.16s\"\n", hdr.Info);
	ft->info.rate = (hdr.Srate < 240) ? 16000 : 32000;
	report("DVMS rate %dbit/s using %dbit/s deviation %d%%\n", 
	       hdr.Srate*100, ft->info.rate, 
	       ((ft->info.rate - hdr.Srate*100) * 100) / ft->info.rate);
	cvsdstartread(ft);
	p->swapbits = 0;
}

/* ---------------------------------------------------------------------- */

void dvmsstartwrite(ft) 
ft_t ft;
{
	struct cvsdpriv *p = (struct cvsdpriv *) ft->priv;
	struct dvms_header hdr;
	
	cvsdstartwrite(ft);
	make_dvms_hdr(ft, &hdr);
	dvms_write_header(ft->fp, &hdr);
	if (!ft->seekable)
	       warn("Length in output .DVMS header will wrong since can't seek to fix it");
	p->swapbits = 0;
}

/* ---------------------------------------------------------------------- */

void
dvmsstopwrite(ft)
ft_t ft;
{
	struct dvms_header hdr;
	
	cvsdstopwrite(ft);
	if (!ft->seekable)
		return;
	if (fseek(ft->fp, 0L, 0) != 0)
		fail("Can't rewind output file to rewrite DVMS header.");
	make_dvms_hdr(ft, &hdr);
	dvms_write_header(ft->fp, &hdr);
}

/* ---------------------------------------------------------------------- */
