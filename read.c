#include <stdlib.h>
#include <assert.h>
#include <zlib.h>
#include "pgpriv.h"
#include "kseq.h"
KSTREAM_INIT(gzFile, gzread, 0x10000)

typedef struct {
	int32_t n_exon, m_exon;
	pg_exon_t *exon;
} pg_exons_t;

static inline void pg_add_exon(pg_exons_t *tmp, int32_t st)
{
	pg_exon_t *p;
	if (tmp->n_exon == tmp->m_exon) {
		tmp->m_exon += (tmp->m_exon>>1) + 16;
		tmp->exon = PG_REALLOC(pg_exon_t, tmp->exon, tmp->m_exon);
	}
	p = &tmp->exon[tmp->n_exon++];
	p->ost = p->oen = st;
}

static void pg_parse_cigar(pg_data_t *d, pg_genome_t *g, pg_hit_t *hit, pg_exons_t *tmp, const char *cg)
{
	const char *p = cg;
	char *r;
	int64_t x = 0;
	int32_t i, n_fs = 0;
	pg_exon_t *t;
	tmp->n_exon = 0;
	pg_add_exon(tmp, 0);
	while (p) {
		int64_t l;
		l = strtol(p, &r, 10);
		if (*r == 'N' || *r == 'U' || *r == 'V') {
			int64_t st, en;
			if (*r == 'N') st = x, en = x + l;
			else if (*r == 'U') st = x + 1, en = x + l - 2;
			else st = x + 2, en = x + l - 1;
			tmp->exon[tmp->n_exon - 1].oen = st;
			tmp->exon[tmp->n_exon - 1].n_fs = n_fs;
			pg_add_exon(tmp, en);
			x += l, n_fs = 0;
		} else if (*r == 'M' || *r == 'X' || *r == '=' || *r == 'D') {
			x += l * 3;
		} else if (*r == 'F' || *r == 'G') {
			x += l, ++n_fs;
		}
	}
	assert(x == hit->ce - hit->cs);
	if (g->n_exon + tmp->n_exon > g->m_exon) {
		g->m_exon = g->n_exon + tmp->n_exon;
		g->m_exon += (g->m_exon>>1) + 16;
		g->exon = PG_REALLOC(pg_exon_t, g->exon, g->m_exon);
	}
	t = &g->exon[g->n_exon];
	if (!hit->rev) {
		memcpy(t, tmp->exon, tmp->n_exon * sizeof(pg_exon_t));
	} else {
		for (i = tmp->n_exon - 1; i >= 0; --i, ++t) {
			t->ost = x - tmp->exon[i].oen;
			t->oen = x - tmp->exon[i].ost;
			t->n_fs = tmp->exon[i].n_fs;
		}
	}
	g->n_exon += tmp->n_exon;
}

int32_t pg_read_paf(pg_data_t *d, const char *fn, int32_t sep)
{
	gzFile fp;
	kstream_t *ks;
	kstring_t str = {0,0,0};
	int32_t dret, absent;
	pg_dict_t *d_ctg;
	pg_genome_t *g;
	pg_exons_t buf = {0,0,0};

	fp = fn && strcmp(fn, "-")? gzopen(fn, "r") : gzdopen(0, "r");
	if (fp == 0) return -1;

	d_ctg = pg_dict_init();
	if (d->n_genome == d->m_genome) {
		d->m_genome += (d->m_genome>>1) + 16;
		d->genome = PG_REALLOC(pg_genome_t, d->genome, d->m_genome);
	}
	g = &d->genome[d->n_genome++];
	memcpy(g, 0, sizeof(*g));

	ks = ks_init(fp);
	while (ks_getuntil(ks, KS_SEP_LINE, &str, &dret) >= 0) {
		char *p, *q, *r;
		int32_t i;
		pg_hit_t hit;
		hit.pid = hit.cid = -1;
		for (p = q = str.s, i = 0;; ++p) {
			if (*p == '\t' || *p == 0) {
				int32_t c = *p;
				if (i == 0) { // query name
					int32_t gid, pid;
					const char *tmp;
					for (r = q; r < p && *r != sep; ++r) {}
					if (*r == sep) {
						*r = 0;
						tmp = pg_dict_put(d->d_gene, q, &gid, 0);
						*r = sep;
					} else {
						tmp = pg_dict_put(d->d_gene, q, &gid, 0);
					}
					tmp = pg_dict_put(d->d_prot, q, &pid, &absent);
					if (absent) { // protein is new
						d->m_prot = (d->m_prot>>1) + 16;
						d->prot = PG_REALLOC(pg_prot_t, d->prot, d->m_prot);
					}
					assert(pid < d->m_prot);
					d->prot[pid].name = tmp;
					d->prot[pid].gid = gid;
					hit.pid = pid;
				} else if (i == 1) {
					d->prot[hit.pid].len = strtol(q, &r, 10);
				} else if (i == 2) {
					hit.qs = strtol(q, &r, 10);
				} else if (i == 3) {
					hit.qe = strtol(q, &r, 10);
				} else if (i == 4) {
					if (*q != '+' && *q != '-') break;
					hit.rev = *q == '+'? 0 : 1;
				} else if (i == 5) {
					int32_t cid;
					pg_dict_put(d_ctg, q, &cid, &absent);
					if (absent) {
						if (g->n_ctg == g->m_ctg) {
							g->m_ctg = (g->m_ctg>>1) + 16;
							g->ctg = PG_REALLOC(pg_ctg_t, g->ctg, g->m_ctg);
						}
						g->ctg[g->n_ctg++].name = pg_dict_put(d->d_ctg, q, 0, 0);
					}
					assert(cid < g->m_ctg);
					hit.cid = cid;
				} else if (i == 6) {
					g->ctg[hit.cid].len = strtol(q, &r, 10);
				} else if (i == 7) {
					hit.cs = strtol(q, &r, 10);
				} else if (i == 8) {
					hit.ce = strtol(q, &r, 10);
				} else if (i == 9)  {
					hit.mlen = strtol(q, &r, 10);
				} else if (i == 10) {
					hit.blen = strtol(q, &r, 10);
				} else if (i >= 12) {
					if (strncmp(q, "ms:i:", 5) == 0) {
						hit.score = strtol(q + 5, &r, 10);
					} else if (strncmp(q, "cg:Z:", 5) == 0) {
						pg_parse_cigar(d, g, &hit, &buf, q + 5);
					}
				}
				q = p + 1, ++i;
				if (c == 0) break;
			}
		}
	}
	pg_dict_destroy(d_ctg);
	ks_destroy(ks);
	gzclose(fp);
	return 0;
}
