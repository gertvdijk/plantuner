/*
 * Copyright (c) 2009 Teodor Sigaev <teodor@sigaev.ru>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of any co-contributors
 *        may be used to endorse or promote products derived from this software
 *        without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY CONTRIBUTORS ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <postgres.h>

#include <fmgr.h>
#include <access/heapam.h>
#include <catalog/namespace.h>
#include <catalog/pg_class.h>
#include <nodes/pg_list.h>
#include <optimizer/plancat.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>

PG_MODULE_MAGIC;

static int 	nIndexesOut = 0;
static Oid	*indexesOut = NULL;
get_relation_info_hook_type	prevHook = NULL;
static bool	fix_empty_table = false;

static char *indexesOutStr = "";

static const char *
indexesOutAssign(const char * newval, bool doit, GucSource source) 
{
	char       *rawname;
	List       *namelist;
	ListCell   *l;
	Oid			*newOids = NULL;
	int			nOids = 0,
				i = 0;

	rawname = pstrdup(newval);

	if (!SplitIdentifierString(rawname, ',', &namelist))
		goto cleanup;

	if (doit) 
	{
		nOids = list_length(namelist);
		newOids = malloc(sizeof(Oid) * (nOids+1));
		if (!newOids)
			elog(ERROR,"could not allocate %d bytes", (int)(sizeof(Oid) * (nOids+1)));
	}

	foreach(l, namelist)
	{
		char     	*curname = (char *) lfirst(l);
		Oid			indexOid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(curname)), true);

		if (indexOid == InvalidOid)
		{
#if PG_VERSION_NUM >= 90100
			if (doit == false)
#endif
				elog(WARNING,"'%s' does not exist", curname);
			continue;
		}
		else if ( get_rel_relkind(indexOid) != RELKIND_INDEX )
		{
#if PG_VERSION_NUM >= 90100
			if (doit == false)
#endif
				elog(WARNING,"'%s' is not an index", curname);
			continue;
		}
		else if (doit)
		{
			newOids[i++] = indexOid;
		}
	}

	if (doit) 
	{
		nIndexesOut = nOids;
		indexesOut = newOids;
	}

	pfree(rawname);
	list_free(namelist);

	return newval;

cleanup:
	if (newOids)
		free(newOids);
	pfree(rawname);
	list_free(namelist);
	return NULL;
}

#if PG_VERSION_NUM >= 90100

static bool
indexesOutCheck(char **newval, void **extra, GucSource source)
{
	char *val;

	val = (char*)indexesOutAssign(*newval, false, source);

	if (val)
	{
		*newval = val;
		return true;
	}

	return false;
}
static void
indexesOutAssignNew(const char *newval, void *extra)
{
	indexesOutAssign(newval, true, PGC_S_USER /* doesn't matter */);
}

#endif

static void
indexFilter(PlannerInfo *root, Oid relationObjectId, bool inhparent, RelOptInfo *rel) {
	int i;

	for(i=0;i<nIndexesOut;i++)
	{
		ListCell   *l;

		foreach(l, rel->indexlist)
		{
			IndexOptInfo	*info = (IndexOptInfo*)lfirst(l);

			if (indexesOut[i] == info->indexoid)
			{
				rel->indexlist = list_delete_ptr(rel->indexlist, info);
				break;
			}
		}
	}

	if (fix_empty_table && rel)
	{
		

	}

}

static void
execPlantuner(PlannerInfo *root, Oid relationObjectId, bool inhparent, RelOptInfo *rel) {
	Relation 	relation;

	relation = heap_open(relationObjectId, NoLock);
	if (relation->rd_rel->relkind == RELKIND_RELATION)
	{
		if (fix_empty_table && RelationGetNumberOfBlocks(relation) == 0)
		{
			/*
			 * estimate_rel_size() could be too pessimistic for particular
			 * workload
			 */
			rel->pages = 0.0;
			rel->tuples = 0.0;
		}

		indexFilter(root, relationObjectId, inhparent, rel);
	}
	heap_close(relation, NoLock);

	/*
	 * Call next hook if it exists 
	 */
	if (prevHook)
		prevHook(root, relationObjectId, inhparent, rel);
}

static const char*
IndexFilterShow(void) 
{
	char 	*val, *ptr;
	int 	i,
			len;

	len = 1 /* \0 */ + nIndexesOut * (2 * NAMEDATALEN + 2 /* ', ' */ + 1 /* . */);
	ptr = val = palloc(len);

	*ptr ='\0';
	for(i=0; i<nIndexesOut; i++)
	{
		char 	*relname = get_rel_name(indexesOut[i]);
		Oid 	nspOid = get_rel_namespace(indexesOut[i]);
		char 	*nspname = get_namespace_name(nspOid); 

		if ( relname == NULL || nspOid == InvalidOid || nspname == NULL )
			continue;

		ptr += snprintf(ptr, len - (ptr - val), "%s%s.%s",
												(i==0) ? "" : ", ",
												nspname,
												relname);
	}

	return val;
}

void _PG_init(void);
void
_PG_init(void) 
{
    DefineCustomStringVariable(
		"plantuner.forbid_index",
		"List of forbidden indexes",
		"Listed indexes will not be used in queries",
		&indexesOutStr,
		"",
		PGC_USERSET,
		0,
#if PG_VERSION_NUM >= 90100
		indexesOutCheck,
		indexesOutAssignNew,
#else
		indexesOutAssign,
#endif
		IndexFilterShow
	);

    DefineCustomBoolVariable(
		"plantuner.fix_empty_table",
		"Sets to zero estimations for empty tables",
		"Sets to zero estimations for empty or newly created tables",
		&fix_empty_table,
#if PG_VERSION_NUM >= 80400
		fix_empty_table,
#endif
		PGC_USERSET,
#if PG_VERSION_NUM >= 80400
		GUC_NOT_IN_SAMPLE,
#if PG_VERSION_NUM >= 90100
		NULL,
#endif
#endif
		NULL,
		NULL
	);

	if (get_relation_info_hook != execPlantuner )
	{
		prevHook = get_relation_info_hook;
		get_relation_info_hook = execPlantuner;
	}
}
