/* ui_app.c - application screens and records: registration and field access (ui_app.h). */
#include <string.h>
#include "ui_app.h"
#include "param.h"

static const ui_app_t *A;
volatile uint32_t ui_last_touch_ms;

void ui_app_set(const ui_app_t *a) { A = a; }
const ui_app_t *ui_app(void) { return A; }

const ui_record_t *ui_app_record(uint16_t id) {
    if (!A) return NULL;
    for (size_t k = 0; k < A->n_records; ++k) if (A->records[k].param_id == id) return &A->records[k];
    return NULL;
}

static size_t elem_size(const ui_field_t *f) {
    switch (f->kind) {
    case RF_BOOL: return 1;
    case RF_TEXT: return f->size;
    case RF_MS_AS_S: return 4;
    case RF_HMS: return 3;
    default: return param_ctype_size(f->ctype);
    }
}

uint16_t ui_field_at(const ui_record_t *r, const ui_field_t *f, unsigned row, unsigned elem) {
    size_t es = elem_size(f);
    size_t a = f->off + (size_t)elem * (r->rows || !f->stride ? es : f->stride);
    if (r->rows) a += (size_t)row * f->stride;
    return (uint16_t)a;
}

static int64_t get_int(const uint8_t *p, uint8_t ct) {
    switch (ct) {
    case CT_BOOL: case CT_U8: return p[0];
    case CT_I8: return (int8_t)p[0];
    case CT_U16: { uint16_t v; memcpy(&v, p, 2); return v; }
    case CT_I16: { int16_t v; memcpy(&v, p, 2); return v; }
    case CT_I32: { int32_t v; memcpy(&v, p, 4); return v; }
    default: { uint32_t v; memcpy(&v, p, 4); return v; }
    }
}

static void set_int(uint8_t *p, uint8_t ct, int64_t v) {
    switch (ct) {
    case CT_BOOL: case CT_U8: case CT_I8: p[0] = (uint8_t)v; break;
    case CT_U16: case CT_I16: { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); break; }
    default: { uint32_t x = (uint32_t)v; memcpy(p, &x, 4); break; }
    }
}

/* the record a field belongs to (for its row layout) */
static const ui_record_t *owner(const ui_field_t *f) {
    if (!A) return NULL;
    for (size_t k = 0; k < A->n_records; ++k)
        if (f >= A->records[k].fields && f < A->records[k].fields + A->records[k].n_fields) return &A->records[k];
    return NULL;
}

bool ui_field_get(const ui_field_t *f, const uint8_t *rec, unsigned row, unsigned elem, int64_t *v) {
    const ui_record_t *r = owner(f);
    if (!r) return false;
    const uint8_t *p = rec + ui_field_at(r, f, row, elem);
    switch (f->kind) {
    case RF_BOOL: *v = p[0] != 0; return true;
    case RF_MS_AS_S: *v = (int64_t)get_int(p, CT_U32); return true;
    case RF_HMS: *v = (int64_t)p[2] * 3600 + (int64_t)p[1] * 60 + p[0]; return true;   /* seconds of the day */
    case RF_TEXT: return false;
    default: *v = get_int(p, f->ctype); return true;
    }
}

bool ui_field_set(const ui_field_t *f, uint8_t *rec, unsigned row, unsigned elem, int64_t v) {
    const ui_record_t *r = owner(f);
    if (!r) return false;
    uint8_t *p = rec + ui_field_at(r, f, row, elem);
    switch (f->kind) {
    case RF_BOOL: p[0] = v ? 1u : 0u; return true;
    case RF_HMS:
        if (v < 0 || v >= 86400) return false;
        p[0] = (uint8_t)(v % 60); p[1] = (uint8_t)(v / 60 % 60); p[2] = (uint8_t)(v / 3600);
        return true;
    case RF_TEXT: return false;
    case RF_MS_AS_S: if (v < f->min || v > ui_field_max(f)) return false; set_int(p, CT_U32, v); return true;
    default:
        if (v < f->min || v > ui_field_max(f)) return false;
        set_int(p, f->ctype, v);
        return true;
    }
}

bool ui_param_value(uint16_t id, uint16_t index, int64_t *v) {
    const param_def_t *d = param_find(id);
    if (!d) return false;
    if (!PARAM_IS_BLOB(d)) {
        plat_val_t x;
        if (!param_read(id, &x)) return false;
        switch (d->ptype) {
        case PT_BOOL: *v = x.b; break;
        case PT_INT: *v = x.i; break;
        case PT_FLOAT: *v = (int64_t)x.f; break;
        default: *v = x.u; break;
        }
        return true;
    }
    if (d->ptype != PT_ARRAY) return false;
    size_t es = param_ctype_size(d->ctype);
    if (index == UW_NO_INDEX) index = 0;
    if ((size_t)(index + 1u) * es > d->size) return false;
    uint8_t b[4] = { 0 };
    if (param_read_blob(id, (uint16_t)(index * es), b, (uint16_t)es) != PERR_OK) return false;
    if (d->ctype == CT_F32) { float fv; memcpy(&fv, b, 4); *v = (int64_t)fv; return true; }
    *v = get_int(b, d->ctype);
    return true;
}

int32_t ui_field_max(const ui_field_t *f) {
    int64_t m;
    if (f->max_param && ui_param_value(f->max_param, UW_NO_INDEX, &m) && m < f->max) return m < f->min ? f->min : (int32_t)m;
    return f->max;
}

unsigned ui_record_rows_used(const ui_record_t *r, const uint8_t *rec) {
    if (!r->rows) return 0;
    if (r->rows_off == UI_ROWS_ALL) return r->rows;
    int64_t n = get_int(rec + r->rows_off, r->rows_ctype);
    return n < 0 ? 0u : n > r->rows ? r->rows : (unsigned)n;
}

void ui_record_set_rows(const ui_record_t *r, uint8_t *rec, unsigned n) {
    if (!r->rows || r->rows_off == UI_ROWS_ALL) return;
    if (n > r->rows) n = r->rows;
    set_int(rec + r->rows_off, r->rows_ctype, n);
}

static bool id_ok(uint16_t id) { return id == 0 || param_find(id) != NULL; }

uint16_t ui_app_check(const ui_app_t *a) {
    if (!a) return 0;
    const ui_app_t *saved = A;
    A = a;                                    /* owner() looks fields up in it */
    uint16_t bad = 0;
    for (size_t k = 0; k < a->n_records && !bad; ++k) {
        const ui_record_t *r = &a->records[k];
        const param_def_t *d = param_find(r->param_id);
        if (!d || !(d->flags & PF_RECORD) || !r->fields || !r->n_fields) { bad = r->param_id ? r->param_id : 0xFFFF; break; }
        if (r->rows && r->rows_off != UI_ROWS_ALL && (size_t)r->rows_off + param_ctype_size(r->rows_ctype) > d->size) { bad = r->param_id; break; }
        for (unsigned i = 0; i < r->n_fields; ++i) {
            const ui_field_t *f = &r->fields[i];
            size_t es = elem_size(f);
            unsigned rows = r->rows ? r->rows : 1u, cnt = r->rows ? 1u : (f->count ? f->count : 1u);
            if (!es || f->kind > RF_ENUM || f->min > f->max || !id_ok(f->max_param) ||
                (r->rows && f->stride < es && rows > 1) ||
                (size_t)ui_field_at(r, f, rows - 1u, cnt - 1u) + es > d->size ||
                (f->kind == RF_ENUM && (!f->items || !f->n_items))) { bad = r->param_id; break; }
        }
    }
    for (size_t s = 0; s < a->n_screens && !bad; ++s) {
        const ui_screen_t *sc = &a->screens[s];
        for (unsigned i = 0; i < sc->n && !bad; ++i) {
            const ui_widget_t *w = &sc->w[i];
            uint16_t ids[] = { w->value_id, w->cmd_id, w->cmd2_id, w->state_id, w->enable_id, w->show_id, w->count_id, w->names_id };
            for (size_t j = 0; j < sizeof ids / sizeof ids[0]; ++j) if (!id_ok(ids[j])) { bad = ids[j]; break; }
            if (bad) break;
            if (w->kind > UW_RECORD) bad = 0xFFFF;
            else if (w->kind == UW_VALUE && !w->value_id) bad = 0xFFFF;
            else if ((w->kind == UW_BUTTON || w->kind == UW_GRID || w->kind == UW_DIALOG) && !w->cmd_id) bad = 0xFFFF;
            else if (w->kind == UW_RECORD && !ui_app_record(w->record_id)) bad = w->record_id ? w->record_id : 0xFFFF;
        }
    }
    A = saved;
    return bad;
}
