//
//  Copyright (C) 2013  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "rt.h"
#include "tree.h"
#include "common.h"
#include "fstapi.h"

#include <assert.h>

static tree_t   fst_top;
static void    *fst_ctx;
static ident_t  fst_data_i;
static ident_t  std_logic_i;
static ident_t  std_ulogic_i;
static ident_t  std_bit_i;
static uint64_t last_time;

typedef struct fst_data fst_data_t;

typedef void (*fst_fmt_fn_t)(tree_t, fst_data_t *);

struct fst_data {
   fstHandle     handle;
   fst_fmt_fn_t  fmt;
   range_kind_t  dir;
   const char   *map;
   size_t        size;
};

static void fst_close(void)
{
   fstWriterEmitTimeChange(fst_ctx, rt_now());
   fstWriterClose(fst_ctx);
}

static void fst_fmt_int(tree_t decl, fst_data_t *data)
{
   uint64_t val;
   rt_signal_value(decl, &val, 1, false);

   char buf[data->size + 1];
   for (size_t i = 0; i < data->size; i++)
      buf[data->size - 1 - i] = (val & (1 << i)) ? '1' : '0';
   buf[data->size] = '\0';

   fstWriterEmitValueChange(fst_ctx, data->handle, buf);
}

static void fst_fmt_chars(tree_t decl, fst_data_t *data)
{
   uint64_t vals[data->size];
   rt_signal_value(decl, vals, data->size, false);

   char buf[data->size + 1];
   for (size_t i = 0; i < data->size; i++)
      buf[i] = data->map[vals[i]];
   buf[data->size] = '\0';

   fstWriterEmitValueChange(fst_ctx, data->handle, buf);
}

static void fst_event_cb(uint64_t now, tree_t decl)
{
   if (now != last_time) {
      fstWriterEmitTimeChange(fst_ctx, now);
      last_time = now;
   }

   fst_data_t *data = tree_attr_ptr(decl, fst_data_i);
   (*data->fmt)(decl, data);
}

static bool fst_can_fmt_chars(type_t type, fst_data_t *data,
                              enum fstVarType *vt,
                              enum fstSupplimentalDataType *sdt)
{
   type_t base = type_base_recur(type);
   ident_t name = type_ident(base);
   if (name == std_ulogic_i) {
      if (type_ident(type) == std_logic_i)
         *sdt = FST_SDT_VHDL_STD_LOGIC;
      else
         *sdt = FST_SDT_VHDL_STD_ULOGIC;
      *vt = FST_VT_SV_LOGIC;
      data->size = 1;
      data->fmt  = fst_fmt_chars;
      data->map  = "UX01ZWLH-";
      return true;;
   }
   else if (name == std_bit_i) {
      *sdt = FST_SDT_VHDL_BIT;
      *vt  = FST_VT_SV_LOGIC;
      data->size = 1;
      data->fmt  = fst_fmt_chars;
      data->map  = "01";
      return true;
   }
   else
      return false;
}

static void fst_walk_scopes(const char *new, const char *old)
{
   if ((*new == '\0') && (*old == '\0'))
      ;
   else if (*new == '\0') {
      fstWriterSetUpscope(fst_ctx);

      const char *next = strchr(old, ':');
      const size_t len = (next != NULL) ? (next - old + 1) : strlen(old);

      fst_walk_scopes(new, old + len);
   }
   else if (*old == '\0') {
      const char *next = strchr(new, ':');
      const size_t nlen = strlen(new);
      const size_t slen = (next != NULL) ? (next - new + 1) : nlen;

      char name[slen + 1];
      strncpy(name, new, (next != NULL) ? (next - new) : nlen);
      name[slen] = '\0';

      fstWriterSetScope(fst_ctx, FST_ST_VHDL_BLOCK, name,
                        "" /* XXX: scopecomp?? */);

      fst_walk_scopes(new + slen, old);
   }
   else {
      const char *next_new = strchr(new, ':');
      const size_t len_new =
         (next_new != NULL) ? (next_new - new + 1) : strlen(new);

      const char *next_old = strchr(old, ':');
      const size_t len_old =
         (next_old != NULL) ? (next_old - old + 1) : strlen(old);

      if ((len_old == len_new) && (strncmp(new, old, len_old) == 0))
         fst_walk_scopes(new + len_new, old + len_old);
      else {
         fst_walk_scopes("", old);
         fst_walk_scopes(new, "");
      }
   }
}

static void fst_enter_scope(tree_t d, char **current)
{
   const char *name = istr(tree_ident(d));

   const char *last_scope = strrchr(name, ':');
   assert(last_scope != NULL);

   const size_t len = last_scope - name - 1;

   char this[len + 1];
   strncpy(this, name + 1, len);
   this[len] = '\0';

   if (strcmp(*current, this) == 0)
      return;

   fst_walk_scopes(this, *current);

   free(*current);
   *current = strdup(this);
}

void fst_restart(void)
{
   if (fst_ctx == NULL)
      return;

   char *current_scope = strdup("");

   const int ndecls = tree_decls(fst_top);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(fst_top, i);
      if (tree_kind(d) != T_SIGNAL_DECL)
         continue;

      type_t type = tree_type(d);

      fst_data_t *data = xmalloc(sizeof(fst_data_t));
      memset(data, '\0', sizeof(fst_data_t));

      enum fstVarType vt;
      enum fstSupplimentalDataType sdt;
      if (type_is_array(type)) {
         type_t elem = type_elem(type);
         if (!fst_can_fmt_chars(elem, data, &vt, &sdt)) {
            warn_at(tree_loc(d), "cannot represent arrays of type %s "
                    "in FST format", type_pp(elem));
            free(data);
            continue;
         }

         range_t r = type_dim(type, 0);

         int64_t low, high;
         range_bounds(r, &low, &high);

         data->dir  = r.kind;
         data->size = high - low + 1;
      }
      else {
         type_t base = type_base_recur(type);
         switch (type_kind(base)) {
         case T_INTEGER:
            sdt  = FST_SDT_VHDL_INTEGER;
            vt   = FST_VT_VCD_INTEGER;
            data->size = 32;
            data->fmt  = fst_fmt_int;
            break;

         case T_ENUM:
            if (fst_can_fmt_chars(type, data, &vt, &sdt))
               break;
            // Fall-through

         default:
            warn_at(tree_loc(d), "cannot represent type %s in FST format",
                    type_pp(type));
            free(data);
            continue;
         }
      }

      fst_enter_scope(d, &current_scope);

      data->handle = fstWriterCreateVar2(
         fst_ctx,
         vt,
         FST_VD_IMPLICIT,
         data->size,
         strrchr(istr(tree_ident(d)), ':') + 1,
         0,
         type_pp(type),
         FST_SVT_VHDL_SIGNAL,
         sdt);

      tree_add_attr_ptr(d, fst_data_i, data);

      rt_set_event_cb(d, fst_event_cb);
   }

   last_time = UINT64_MAX;

   free(current_scope);
}

void fst_init(const char *file, tree_t top)
{
   fst_data_i   = ident_new("fst_data");
   std_logic_i  = ident_new("IEEE.STD_LOGIC_1164.STD_LOGIC");
   std_ulogic_i = ident_new("IEEE.STD_LOGIC_1164.STD_ULOGIC");
   std_bit_i    = ident_new("STD.STANDARD.BIT");

   if ((fst_ctx = fstWriterCreate(file, 1)) == NULL)
      fatal("fstWriterCreate failed");

   fstWriterSetTimescale(fst_ctx, -15);

   atexit(fst_close);

   fst_top = top;
}