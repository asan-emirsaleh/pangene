#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "pgpriv.h"
#include "kalloc.h"

void pg_post_process(const pg_opt_t *opt, pg_data_t *d)
{
	int32_t i;
	if (pg_verbose >= 3)
		fprintf(stderr, "[M::%s::%s] %d genes and %d proteins\n", __func__, pg_timestamp(), d->n_gene, d->n_prot);
	pg_flag_primary(d);
	for (i = 0; i < d->n_genome; ++i) {
		pg_genome_t *g = &d->genome[i];
		int32_t n_pseudo, n_shadow;
		n_pseudo = pg_flag_pseudo(0, d->prot, g);
		pg_hit_sort(0, g, 0);
		n_shadow = pg_flag_shadow(opt, d->prot, g, 0, 0);
		if (pg_verbose >= 3)
			fprintf(stderr, "[M::%s::%s] genome %d: %d pseudo, %d shadow\n", __func__, pg_timestamp(), i, n_pseudo, n_shadow);
	}
}

pg_graph_t *pg_graph_init(pg_data_t *d)
{
	pg_graph_t *g;
	g = PG_CALLOC(pg_graph_t, 1);
	g->d = d;
	g->vtx = PG_CALLOC(pg_vtx_t, d->n_gene);
	return g;
}

void pg_graph_destroy(pg_graph_t *q)
{
	free(q->g2v); free(q->vtx); free(q->arc);
	free(q);
}

static void pg_gen_vertex_aux(void *km, const pg_opt_t *opt, const pg_data_t *d, pg_genome_t *g, uint64_t *cnt)
{
	int32_t i;
	int8_t *flag;
	flag = Kcalloc(km, int8_t, d->n_gene);
	for (i = 0; i < g->n_hit; ++i) {
		const pg_hit_t *a = &g->hit[i];
		int32_t gid;
		if (a->rank != 0) continue;
		gid = d->prot[a->pid].gid;
		if (a->shadow == 0) flag[gid] |= 1;
		else flag[gid] |= 2;
	}
	for (i = 0; i < d->n_gene; ++i) {
		if (flag[i]&1) cnt[i] += 1ULL<<32;
		else if (flag[i]&2) cnt[i] += 1ULL;
	}
	kfree(km, flag);
}

void pg_gen_vtx(const pg_opt_t *opt, pg_graph_t *q)
{
	const pg_data_t *d = q->d;
	int32_t i;
	uint64_t *cnt;
	cnt = PG_CALLOC(uint64_t, d->n_gene);
	for (i = 0; i < d->n_genome; ++i)
		pg_gen_vertex_aux(0, opt, d, &d->genome[i], cnt);
	for (i = 0; i < d->n_gene; ++i) {
		int32_t pri = cnt[i]>>32, sec = (int32_t)cnt[i];
		if (pri >= d->n_genome * opt->min_vertex_ratio) {
			pg_vtx_t *p;
			PG_EXTEND(pg_vtx_t, q->vtx, q->n_vtx, q->m_vtx);
			p = &q->vtx[q->n_vtx++];
			p->gid = i, p->pri = pri, p->sec = sec;
		}
	}
	free(cnt);
	q->g2v = PG_MALLOC(int32_t, d->n_gene);
	for (i = 0; i < d->n_gene; ++i)
		q->g2v[i] = -1;
	for (i = 0; i < q->n_vtx; ++i)
		q->g2v[q->vtx[i].gid] = i;
	if (pg_verbose >= 3)
		fprintf(stderr, "[M::%s::%s] selected %d vertices out of %d genes\n", __func__, pg_timestamp(), q->n_vtx, q->d->n_gene);
}

void pg_graph_flag_vtx(pg_graph_t *q)
{
	int32_t j, i;
	for (j = 0; j < q->d->n_genome; ++j) {
		pg_genome_t *g = &q->d->genome[j];
		for (i = 0; i < g->n_hit; ++i)
			if (q->g2v[q->d->prot[g->hit[i].pid].gid] >= 0)
				g->hit[i].vtx = 1;
	}
}

void pg_gen_arc(const pg_opt_t *opt, pg_graph_t *q)
{
	int32_t j, i, i0;
	int64_t n_arc = 0, m_arc = 0, n_arc1 = 0, m_arc1 = 0;
	pg128_t *p, *arc = 0, *arc1 = 0;
	for (j = 0; j < q->d->n_genome; ++j) {
		pg_genome_t *g = &q->d->genome[j];
		uint32_t w, v = (uint32_t)-1;
		int64_t vpos = -1;
		int32_t vcid = -1;
		pg_flag_shadow(opt, q->d->prot, g, 1, 1); // this requires sorting by pg_hit_t::cs
		pg_hit_sort(0, g, 1); // sort by pg_hit_t::cm
		n_arc1 = 0;
		for (i = 0; i < g->n_hit; ++i) {
			const pg_hit_t *a = &g->hit[i];
			if (!a->pri || a->shadow || !a->vtx) continue;
			w = (uint32_t)q->d->prot[a->pid].gid<<1 | a->rev;
			if (a->cid != vcid) v = (uint32_t)-1, vpos = -1;
			if (v != (uint32_t)-1) {
				PG_EXTEND0(pg128_t, arc1, n_arc1, m_arc1);
				p = &arc1[n_arc1++], p->x = (uint64_t)v<<32|w, p->y = a->cm - vpos;
				PG_EXTEND0(pg128_t, arc1, n_arc1, m_arc1);
				p = &arc1[n_arc1++], p->x = (uint64_t)(w^1)<<32|(v^1), p->y = a->cm - vpos;
			}
			v = w, vpos = a->cm, vcid = a->cid;
		}
		pg_hit_sort(0, g, 0); // sort by pg_hit_t::cs
		assert(n_arc1 <= INT32_MAX);
		radix_sort_pg128x(arc1, arc1 + n_arc1);
		for (i = 1, i0 = 0; i <= n_arc1; ++i) {
			if (i == n_arc1 || arc1[i0].x != arc1[i].x) {
				int32_t j;
				uint64_t dist = 0;
				for (j = i0; j < i; ++j)
					dist += arc1[j].y;
				PG_EXTEND0(pg128_t, arc, n_arc, m_arc);
				p = &arc[n_arc++];
				p->x = arc1[i0].x;
				p->y = (uint64_t)(i - i0) << 32 | (uint64_t)((double)dist / (i - i0) + .499);
				i0 = i;
			}
		}
	}
	free(arc1);
	assert(n_arc <= INT32_MAX);
	radix_sort_pg128x(arc, arc + n_arc);
	q->n_arc = 0;
	for (i0 = 0, i = 1; i <= n_arc; ++i) {
		if (i == n_arc || arc[i].x != arc[i0].x) {
			int64_t dist = 0;
			int32_t n = 0;
			pg_arc_t *p;
			for (j = i0; j < i; ++j)
				n += arc[j].y>>32, dist += ((int32_t)arc[j].y) * (arc[j].y>>32);
			assert(dist >= 0);
			PG_EXTEND0(pg_arc_t, q->arc, q->n_arc, q->m_arc);
			p = &q->arc[q->n_arc++];
			p->x = arc[i0].x;
			p->n_genome = i - i0;
			p->tot_cnt = n;
			p->avg_dist = (int64_t)((double)dist / n + .499);
			i0 = i;
		}
	}
	free(arc);
}

void pg_graph_gen(const pg_opt_t *opt, pg_graph_t *q)
{
	pg_gen_vtx(opt, q);
	pg_graph_flag_vtx(q);
	pg_gen_arc(opt, q);
}
