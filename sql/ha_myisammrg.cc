/* Copyright (c) 2000-2008 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mysql_priv.h"
#include <m_ctype.h>
#include "ha_myisammrg.h"
#ifndef MASTER
#include "../srclib/myisammrg/myrg_def.h"
#else
#include "../myisammrg/myrg_def.h"
#endif

/*****************************************************************************
** MyISAM MERGE tables
*****************************************************************************/

/* MyISAM MERGE handlerton */

handlerton myisammrg_hton= {
  "MRG_MYISAM",
  SHOW_OPTION_YES,
  "Collection of identical MyISAM tables", 
  DB_TYPE_MRG_MYISAM,
  NULL,
  0,       /* slot */
  0,       /* savepoint size. */
  NULL,    /* close_connection */
  NULL,    /* savepoint */
  NULL,    /* rollback to savepoint */
  NULL,    /* release savepoint */
  NULL,    /* commit */
  NULL,    /* rollback */
  NULL,    /* prepare */
  NULL,    /* recover */
  NULL,    /* commit_by_xid */
  NULL,    /* rollback_by_xid */
  NULL,    /* create_cursor_read_view */
  NULL,    /* set_cursor_read_view */
  NULL,    /* close_cursor_read_view */
  HTON_CAN_RECREATE
};


ha_myisammrg::ha_myisammrg(TABLE *table_arg)
  :handler(&myisammrg_hton, table_arg), file(0)
{}

static const char *ha_myisammrg_exts[] = {
  ".MRG",
  NullS
};
extern int table2myisam(TABLE *table_arg, MI_KEYDEF **keydef_out,
                        MI_COLUMNDEF **recinfo_out, uint *records_out);
extern int check_definition(MI_KEYDEF *t1_keyinfo, MI_COLUMNDEF *t1_recinfo,
                            uint t1_keys, uint t1_recs,
                            MI_KEYDEF *t2_keyinfo, MI_COLUMNDEF *t2_recinfo,
                            uint t2_keys, uint t2_recs, bool strict);
static void split_file_name(const char *file_name,
			    LEX_STRING *db, LEX_STRING *name);


extern "C" void myrg_print_wrong_table(const char *table_name)
{
  LEX_STRING db= {NULL, 0}, name;
  char buf[FN_REFLEN];
  split_file_name(table_name, &db, &name);
  memcpy(buf, db.str, db.length);
  buf[db.length]= '.';
  memcpy(buf + db.length + 1, name.str, name.length);
  buf[db.length + name.length + 1]= 0;
  push_warning_printf(current_thd, MYSQL_ERROR::WARN_LEVEL_ERROR,
                      ER_ADMIN_WRONG_MRG_TABLE, ER(ER_ADMIN_WRONG_MRG_TABLE),
                      buf);
}


const char **ha_myisammrg::bas_ext() const
{
  return ha_myisammrg_exts;
}


const char *ha_myisammrg::index_type(uint key_number)
{
  return ((table->key_info[key_number].flags & HA_FULLTEXT) ? 
	  "FULLTEXT" :
	  (table->key_info[key_number].flags & HA_SPATIAL) ?
	  "SPATIAL" :
	  (table->key_info[key_number].algorithm == HA_KEY_ALG_RTREE) ?
	  "RTREE" :
	  "BTREE");
}


int ha_myisammrg::open(const char *name, int mode, uint test_if_locked)
{
  MI_KEYDEF *keyinfo;
  MI_COLUMNDEF *recinfo;
  MYRG_TABLE *u_table;
  uint recs;
  uint keys= table->s->keys;
  int error;
  char name_buff[FN_REFLEN];

  DBUG_PRINT("info", ("ha_myisammrg::open"));
  if (!(file=myrg_open(fn_format(name_buff,name,"","",2 | 4), mode,
		       test_if_locked)))
  {
    DBUG_PRINT("info", ("ha_myisammrg::open exit %d", my_errno));
    return (my_errno ? my_errno : -1);
  }
  DBUG_PRINT("info", ("ha_myisammrg::open myrg_extrafunc..."));
  myrg_extrafunc(file, query_cache_invalidate_by_MyISAM_filename_ref);
  if (!(test_if_locked == HA_OPEN_WAIT_IF_LOCKED ||
	test_if_locked == HA_OPEN_ABORT_IF_LOCKED))
    myrg_extra(file,HA_EXTRA_NO_WAIT_LOCK,0);
  info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);
  if (!(test_if_locked & HA_OPEN_WAIT_IF_LOCKED))
    myrg_extra(file,HA_EXTRA_WAIT_LOCK,0);

  if (table->s->reclength != mean_rec_length && mean_rec_length)
  {
    DBUG_PRINT("error",("reclength: %lu  mean_rec_length: %lu",
			table->s->reclength, mean_rec_length));
    if (test_if_locked & HA_OPEN_FOR_REPAIR)
      myrg_print_wrong_table(file->open_tables->table->filename);
    error= HA_ERR_WRONG_MRG_TABLE_DEF;
    goto err;
  }
  if ((error= table2myisam(table, &keyinfo, &recinfo, &recs)))
  {
    /* purecov: begin inspected */
    DBUG_PRINT("error", ("Failed to convert TABLE object to MyISAM "
                         "key and column definition"));
    goto err;
    /* purecov: end */
  }
  for (u_table= file->open_tables; u_table < file->end_table; u_table++)
  {
    if (check_definition(keyinfo, recinfo, keys, recs,
                         u_table->table->s->keyinfo, u_table->table->s->rec,
                         u_table->table->s->base.keys,
                         u_table->table->s->base.fields, false))
    {
      error= HA_ERR_WRONG_MRG_TABLE_DEF;
      if (test_if_locked & HA_OPEN_FOR_REPAIR)
        myrg_print_wrong_table(u_table->table->filename);
      else
      {
        my_free((gptr) recinfo, MYF(0));
        goto err;
      }
    }
  }
  my_free((gptr) recinfo, MYF(0));
  if (error == HA_ERR_WRONG_MRG_TABLE_DEF)
    goto err;
#if !defined(BIG_TABLES) || SIZEOF_OFF_T == 4
  /* Merge table has more than 2G rows */
  if (table->s->crashed)
  {
    error= HA_ERR_WRONG_MRG_TABLE_DEF;
    goto err;
  }
#endif
  return (0);
err:
  myrg_close(file);
  file=0;
  return (my_errno= error);
}

int ha_myisammrg::close(void)
{
  return myrg_close(file);
}

int ha_myisammrg::write_row(byte * buf)
{
  statistic_increment(table->in_use->status_var.ha_write_count,&LOCK_status);

  if (file->merge_insert_method == MERGE_INSERT_DISABLED || !file->tables)
    return (HA_ERR_TABLE_READONLY);

  if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_INSERT)
    table->timestamp_field->set_time();
  if (table->next_number_field && buf == table->record[0])
  {
    int error;
    if ((error= update_auto_increment()))
      return error;
  }
  return myrg_write(file,buf);
}

int ha_myisammrg::update_row(const byte * old_data, byte * new_data)
{
  statistic_increment(table->in_use->status_var.ha_update_count,&LOCK_status);
  if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_UPDATE)
    table->timestamp_field->set_time();
  return myrg_update(file,old_data,new_data);
}

int ha_myisammrg::delete_row(const byte * buf)
{
  statistic_increment(table->in_use->status_var.ha_delete_count,&LOCK_status);
  return myrg_delete(file,buf);
}

int ha_myisammrg::index_read(byte * buf, const byte * key,
			  uint key_len, enum ha_rkey_function find_flag)
{
  statistic_increment(table->in_use->status_var.ha_read_key_count,
		      &LOCK_status);
  int error=myrg_rkey(file,buf,active_index, key, key_len, find_flag);
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

int ha_myisammrg::index_read_idx(byte * buf, uint index, const byte * key,
				 uint key_len, enum ha_rkey_function find_flag)
{
  statistic_increment(table->in_use->status_var.ha_read_key_count,
		      &LOCK_status);
  int error=myrg_rkey(file,buf,index, key, key_len, find_flag);
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

int ha_myisammrg::index_read_last(byte * buf, const byte * key, uint key_len)
{
  statistic_increment(table->in_use->status_var.ha_read_key_count,
		      &LOCK_status);
  int error=myrg_rkey(file,buf,active_index, key, key_len,
		      HA_READ_PREFIX_LAST);
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

int ha_myisammrg::index_next(byte * buf)
{
  statistic_increment(table->in_use->status_var.ha_read_next_count,
		      &LOCK_status);
  int error=myrg_rnext(file,buf,active_index);
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

int ha_myisammrg::index_prev(byte * buf)
{
  statistic_increment(table->in_use->status_var.ha_read_prev_count,
		      &LOCK_status);
  int error=myrg_rprev(file,buf, active_index);
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

int ha_myisammrg::index_first(byte * buf)
{
  statistic_increment(table->in_use->status_var.ha_read_first_count,
		      &LOCK_status);
  int error=myrg_rfirst(file, buf, active_index);
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

int ha_myisammrg::index_last(byte * buf)
{
  statistic_increment(table->in_use->status_var.ha_read_last_count,
		      &LOCK_status);
  int error=myrg_rlast(file, buf, active_index);
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

int ha_myisammrg::index_next_same(byte * buf,
                                  const byte *key __attribute__((unused)),
                                  uint length __attribute__((unused)))
{
  int error;
  statistic_increment(table->in_use->status_var.ha_read_next_count,
                      &LOCK_status);
  do
  {
    error= myrg_rnext_same(file,buf);
  } while (error == HA_ERR_RECORD_DELETED);
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

int ha_myisammrg::rnd_init(bool scan)
{
  return myrg_extra(file,HA_EXTRA_RESET,0);
}

int ha_myisammrg::rnd_next(byte *buf)
{
  statistic_increment(table->in_use->status_var.ha_read_rnd_next_count,
		      &LOCK_status);
  int error=myrg_rrnd(file, buf, HA_OFFSET_ERROR);
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

int ha_myisammrg::rnd_pos(byte * buf, byte *pos)
{
  statistic_increment(table->in_use->status_var.ha_read_rnd_count,
		      &LOCK_status);
  int error=myrg_rrnd(file, buf, my_get_ptr(pos,ref_length));
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

void ha_myisammrg::position(const byte *record)
{
  ulonglong row_position= myrg_position(file);
  my_store_ptr(ref, ref_length, (my_off_t) row_position);
}


ha_rows ha_myisammrg::records_in_range(uint inx, key_range *min_key,
                                       key_range *max_key)
{
  return (ha_rows) myrg_records_in_range(file, (int) inx, min_key, max_key);
}


int ha_myisammrg::info(uint flag)
{
  MYMERGE_INFO mrg_info;
  (void) myrg_status(file,&mrg_info,flag);
  /*
    The following fails if one has not compiled MySQL with -DBIG_TABLES
    and one has more than 2^32 rows in the merge tables.
  */
  records = (ha_rows) mrg_info.records;
  deleted = (ha_rows) mrg_info.deleted;
#if !defined(BIG_TABLES) || SIZEOF_OFF_T == 4
  if ((mrg_info.records >= (ulonglong) 1 << 32) ||
      (mrg_info.deleted >= (ulonglong) 1 << 32))
    table->s->crashed= 1;
#endif
  data_file_length=mrg_info.data_file_length;
  errkey  = mrg_info.errkey;
  table->s->keys_in_use.set_prefix(table->s->keys);
  table->s->db_options_in_use= mrg_info.options;
  table->s->is_view= 1;
  mean_rec_length= mrg_info.reclength;
  
  /* 
    The handler::block_size is used all over the code in index scan cost
    calculations. It is used to get number of disk seeks required to
    retrieve a number of index tuples.
    If the merge table has N underlying tables, then (assuming underlying
    tables have equal size, the only "simple" approach we can use)
    retrieving X index records from a merge table will require N times more
    disk seeks compared to doing the same on a MyISAM table with equal
    number of records.
    In the edge case (file_tables > myisam_block_size) we'll get
    block_size==0, and index calculation code will act as if we need one
    disk seek to retrieve one index tuple.

    TODO: In 5.2 index scan cost calculation will be factored out into a
    virtual function in class handler and we'll be able to remove this hack.
  */
  block_size= 0;
  if (file->tables)
    block_size= myisam_block_size / file->tables;
  
  update_time=0;
#if SIZEOF_OFF_T > 4
  ref_length=6;					// Should be big enough
#else
  ref_length=4;					// Can't be > than my_off_t
#endif
  if (flag & HA_STATUS_CONST)
  {
    if (table->s->key_parts && mrg_info.rec_per_key)
    {
#ifdef HAVE_purify
      /*
        valgrind may be unhappy about it, because optimizer may access values
        between file->keys and table->key_parts, that will be uninitialized.
        It's safe though, because even if opimizer will decide to use a key
        with such a number, it'll be an error later anyway.
      */
      bzero((char*) table->key_info[0].rec_per_key,
            sizeof(table->key_info[0].rec_per_key[0]) * table->s->key_parts);
#endif
      memcpy((char*) table->key_info[0].rec_per_key,
	     (char*) mrg_info.rec_per_key,
             sizeof(table->key_info[0].rec_per_key[0]) *
             min(file->keys, table->s->key_parts));
    }
  }
  return 0;
}


int ha_myisammrg::extra(enum ha_extra_function operation)
{
  /* As this is just a mapping, we don't have to force the underlying
     tables to be closed */
  if (operation == HA_EXTRA_FORCE_REOPEN ||
      operation == HA_EXTRA_PREPARE_FOR_DELETE)
    return 0;
  return myrg_extra(file,operation,0);
}


/* To be used with WRITE_CACHE, EXTRA_CACHE and BULK_INSERT_BEGIN */

int ha_myisammrg::extra_opt(enum ha_extra_function operation, ulong cache_size)
{
  if ((specialflag & SPECIAL_SAFE_MODE) && operation == HA_EXTRA_WRITE_CACHE)
    return 0;
  return myrg_extra(file, operation, (void*) &cache_size);
}

int ha_myisammrg::external_lock(THD *thd, int lock_type)
{
  return myrg_lock_database(file,lock_type);
}

uint ha_myisammrg::lock_count(void) const
{
  return file->tables;
}


THR_LOCK_DATA **ha_myisammrg::store_lock(THD *thd,
					 THR_LOCK_DATA **to,
					 enum thr_lock_type lock_type)
{
  MYRG_TABLE *open_table;

  for (open_table=file->open_tables ;
       open_table != file->end_table ;
       open_table++)
  {
    *(to++)= &open_table->table->lock;
    if (lock_type != TL_IGNORE && open_table->table->lock.type == TL_UNLOCK)
      open_table->table->lock.type=lock_type;
  }
  return to;
}


/* Find out database name and table name from a filename */

static void split_file_name(const char *file_name,
			    LEX_STRING *db, LEX_STRING *name)
{
  uint dir_length, prefix_length;
  char buff[FN_REFLEN];

  db->length= 0;
  strmake(buff, file_name, sizeof(buff)-1);
  dir_length= dirname_length(buff);
  if (dir_length > 1)
  {
    /* Get database */
    buff[dir_length-1]= 0;			// Remove end '/'
    prefix_length= dirname_length(buff);
    db->str= (char*) file_name+ prefix_length;
    db->length= dir_length - prefix_length -1;
  }
  name->str= (char*) file_name+ dir_length;
  name->length= (uint) (fn_ext(name->str) - name->str);
}


void ha_myisammrg::update_create_info(HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_myisammrg::update_create_info");

  if (!(create_info->used_fields & HA_CREATE_USED_UNION))
  {
    MYRG_TABLE *open_table;
    THD *thd=current_thd;

    create_info->merge_list.next= &create_info->merge_list.first;
    create_info->merge_list.elements=0;

    for (open_table=file->open_tables ;
	 open_table != file->end_table ;
	 open_table++)
    {
      TABLE_LIST *ptr;
      LEX_STRING db, name;

      if (!(ptr = (TABLE_LIST *) thd->calloc(sizeof(TABLE_LIST))))
	goto err;
      split_file_name(open_table->table->filename, &db, &name);
      if (!(ptr->table_name= thd->strmake(name.str, name.length)))
	goto err;
      if (db.length && !(ptr->db= thd->strmake(db.str, db.length)))
	goto err;

      create_info->merge_list.elements++;
      (*create_info->merge_list.next) = (byte*) ptr;
      create_info->merge_list.next= (byte**) &ptr->next_local;
    }
    *create_info->merge_list.next=0;
  }
  if (!(create_info->used_fields & HA_CREATE_USED_INSERT_METHOD))
  {
    create_info->merge_insert_method = file->merge_insert_method;
  }
  DBUG_VOID_RETURN;

err:
  create_info->merge_list.elements=0;
  create_info->merge_list.first=0;
  DBUG_VOID_RETURN;
}


int ha_myisammrg::create(const char *name, register TABLE *form,
			 HA_CREATE_INFO *create_info)
{
  char buff[FN_REFLEN];
  const char **table_names, **pos;
  TABLE_LIST *tables= (TABLE_LIST*) create_info->merge_list.first;
  THD *thd= current_thd;
  uint dirlgt= dirname_length(name);
  DBUG_ENTER("ha_myisammrg::create");

  if (!(table_names= (const char**)
        thd->alloc((create_info->merge_list.elements+1) * sizeof(char*))))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  for (pos= table_names; tables; tables= tables->next_local)
  {
    const char *table_name;
    TABLE **tbl= 0;
    if (create_info->options & HA_LEX_CREATE_TMP_TABLE)
      tbl= find_temporary_table(thd, tables->db, tables->table_name);
    if (!tbl)
    {
      /*
        Construct the path to the MyISAM table. Try to meet two conditions:
        1.) Allow to include MyISAM tables from different databases, and
        2.) allow for moving DATADIR around in the file system.
        The first means that we need paths in the .MRG file. The second
        means that we should not have absolute paths in the .MRG file.
        The best, we can do, is to use 'mysql_data_home', which is '.'
        in mysqld and may be an absolute path in an embedded server.
        This means that it might not be possible to move the DATADIR of
        an embedded server without changing the paths in the .MRG file.
      */
      uint length= my_snprintf(buff, FN_REFLEN, "%s/%s/%s", mysql_data_home,
			       tables->db, tables->table_name);
      /*
        If a MyISAM table is in the same directory as the MERGE table,
        we use the table name without a path. This means that the
        DATADIR can easily be moved even for an embedded server as long
        as the MyISAM tables are from the same database as the MERGE table.
      */
      if ((dirname_length(buff) == dirlgt) && ! memcmp(buff, name, dirlgt))
        table_name= tables->table_name;
      else
        if (! (table_name= thd->strmake(buff, length)))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    else
      table_name= (*tbl)->s->path;
    *pos++= table_name;
  }
  *pos=0;
  DBUG_RETURN(myrg_create(fn_format(buff,name,"","",2+4+16),
			  table_names,
                          create_info->merge_insert_method,
                          (my_bool) 0));
}


void ha_myisammrg::append_create_info(String *packet)
{
  const char *current_db;
  uint db_length;
  THD *thd= current_thd;

  if (file->merge_insert_method != MERGE_INSERT_DISABLED)
  {
    packet->append(STRING_WITH_LEN(" INSERT_METHOD="));
    packet->append(get_type(&merge_insert_method,file->merge_insert_method-1));
  }
  /*
    There is no sence adding UNION clause in case there is no underlying
    tables specified.
  */
  if (file->open_tables == file->end_table)
    return;
  packet->append(STRING_WITH_LEN(" UNION=("));
  MYRG_TABLE *open_table,*first;

  current_db= table->s->db;
  db_length= (uint) strlen(current_db);

  for (first=open_table=file->open_tables ;
       open_table != file->end_table ;
       open_table++)
  {
    LEX_STRING db, name;
    split_file_name(open_table->table->filename, &db, &name);
    if (open_table != first)
      packet->append(',');
    /* Report database for mapped table if it isn't in current database */
    if (db.length &&
	(db_length != db.length ||
	 strncmp(current_db, db.str, db.length)))
    {
      append_identifier(thd, packet, db.str, db.length);
      packet->append('.');
    }
    append_identifier(thd, packet, name.str, name.length);
  }
  packet->append(')');
}


int ha_myisammrg::check(THD* thd, HA_CHECK_OPT* check_opt)
{
  return HA_ADMIN_OK;
}
