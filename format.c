#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <ctype.h>
#include "pgpriv.h"

static inline void str_enlarge(kstring_t *s, int l)
{
	if (s->l + l + 1 > s->m) {
		s->m = s->l + l + 1;
		kroundup32(s->m);
		s->s = PG_REALLOC(char, s->s, s->m);
	}
}

static inline void str_copy(kstring_t *s, const char *st, const char *en)
{
	str_enlarge(s, en - st);
	memcpy(&s->s[s->l], st, en - st);
	s->l += en - st;
}

void pg_sprintf_lite(kstring_t *s, const char *fmt, ...)
{
	char buf[32]; // for integer to string conversion
	const char *p, *q;
	va_list ap;
	va_start(ap, fmt);
	for (q = p = fmt; *p; ++p) {
		if (*p == '%') {
			if (p > q) str_copy(s, q, p);
			++p;
			if (*p == 'd') {
				int c, i, l = 0;
				unsigned int x;
				c = va_arg(ap, int);
				x = c >= 0? c : -c;
				do { buf[l++] = x%10 + '0'; x /= 10; } while (x > 0);
				if (c < 0) buf[l++] = '-';
				str_enlarge(s, l);
				for (i = l - 1; i >= 0; --i) s->s[s->l++] = buf[i];
			} else if (*p == 'l' && *(p+1) == 'd') {
				int c, i, l = 0;
				unsigned long x;
				c = va_arg(ap, long);
				x = c >= 0? c : -c;
				do { buf[l++] = x%10 + '0'; x /= 10; } while (x > 0);
				if (c < 0) buf[l++] = '-';
				str_enlarge(s, l);
				for (i = l - 1; i >= 0; --i) s->s[s->l++] = buf[i];
				++p;
			} else if (*p == 'u') {
				int i, l = 0;
				uint32_t x;
				x = va_arg(ap, uint32_t);
				do { buf[l++] = x%10 + '0'; x /= 10; } while (x > 0);
				str_enlarge(s, l);
				for (i = l - 1; i >= 0; --i) s->s[s->l++] = buf[i];
			} else if (*p == 's') {
				char *r = va_arg(ap, char*);
				str_copy(s, r, r + strlen(r));
			} else if (*p == 'c') {
				str_enlarge(s, 1);
				s->s[s->l++] = va_arg(ap, int);
			} else {
				fprintf(stderr, "ERROR: unrecognized type '%%%c'\n", *p);
				abort();
			}
			q = p + 1;
		}
	}
	if (p > q) str_copy(s, q, p);
	va_end(ap);
	s->s[s->l] = 0;
}

void pg_write_bed1(kstring_t *out, const pg_data_t *d, int32_t aid, int32_t hid)
{
	const pg_genome_t *g = &d->genome[aid];
	const pg_hit_t *a = &g->hit[hid];
	int32_t i;
	pg_sprintf_lite(out, "%s\t%ld\t%ld\t%s\t%d\t%c\t", g->ctg[a->cid].name, a->cs, a->ce, d->prot[a->pid].name, a->score, "+-"[a->rev]);
	pg_sprintf_lite(out, "%ld\t%ld\t0\t%d\t", a->cs, a->ce, a->n_exon);
	for (i = 0; i < a->n_exon; ++i)
		pg_sprintf_lite(out, "%d,", g->exon[a->off_exon + i].oe - g->exon[a->off_exon + i].os);
	pg_sprintf_lite(out, "\t");
	for (i = 0; i < a->n_exon; ++i) {
		#if 1
		pg_sprintf_lite(out, "%d,", g->exon[a->off_exon + i].os);
		#else
		pg_sprintf_lite(out, "%ld,", a->cs + g->exon[a->off_exon + i].os); // for debugging only
		#endif
	}
	pg_sprintf_lite(out, "\trk:i:%d\tfs:i:%d\tcm:i:%ld\n", a->rank, a->fs, a->cm);
}

void pg_write_bed(const pg_data_t *d, int32_t aid)
{
	int32_t i;
	const pg_genome_t *g;
	kstring_t out = {0,0,0};
	assert(aid < d->n_genome);
	g = &d->genome[aid];
	for (i = 0; i < g->n_hit; ++i) {
		out.l = 0;
		pg_write_bed1(&out, d, aid, i);
		fwrite(out.s, 1, out.l, stdout);
	}
	free(out.s);
}
