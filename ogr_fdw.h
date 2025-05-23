/*-------------------------------------------------------------------------
 *
 * ogr_fdw.h
 *		  foreign-data wrapper for GIS data access.
 *
 * Copyright (c) 2014-2015, Paul Ramsey <pramsey@cleverelephant.ca>
 *
 *-------------------------------------------------------------------------
 */

#ifndef _OGR_FDW_H
#define _OGR_FDW_H 1

#define OGR_FDW_RELEASE_NAME "1.1"

/*
 * PostgreSQL
 */
#include "postgres.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "access/tupdesc.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/extension.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#if PG_VERSION_NUM < 120000
#include "nodes/relation.h"
#include "optimizer/var.h"
#else
#include "executor/tuptable.h"
#include "optimizer/appendinfo.h"
#endif

#ifdef PACKAGE_URL
#undef PACKAGE_URL
#endif

/* GDAL/OGR includes and compat */
#include "ogr_fdw_gdal.h"
#include "ogr_fdw_common.h"

/* Local configuration defines */

/* Use hexwkb input by default, but have option to use */
/* the binary recv input instead. Binary input is strict */
/* on geometry structure (no unclosed polys, etc) and */
/* hexwkb is not. */
#define OGR_FDW_HEXWKB TRUE

extern Oid GEOMETRYOID;

typedef enum
{
	OGR_UNMATCHED,
	OGR_GEOMETRY,
	OGR_FID,
	OGR_FIELD
} OgrColumnVariant;

typedef enum {
	OGR_UPDATEABLE_FALSE,
	OGR_UPDATEABLE_TRUE,
	OGR_UPDATEABLE_UNSET,
	OGR_UPDATEABLE_TRY
} OgrUpdateable;

typedef struct OgrFdwColumn
{
	/* PgSQL metadata */
	int   pgattnum;          /* PgSQL attribute number */
	int   pgattisdropped;    /* PgSQL attribute dropped? */
	char* pgname;            /* PgSQL column name */
	Oid   pgtype;            /* PgQL data type */
	int   pgtypmod;          /* PgSQL type modifier */

	bool  pgisarray;
	Oid   pgelmtype;         /* If column is array then this is nonzero */

	/* For reading. If array, for array element type. */
	Oid pginputfunc;         /* PgSQL convert cstring to type */
	Oid pginputioparam;
	Oid pgrecvfunc;          /* PgSQL convert binary to type */
	Oid pgrecvioparam;

	/* For writing. If array, for array element type. */
	Oid  pgoutputfunc;       /* PgSQL convert type to cstring */
	bool pgoutputvarlena;
	Oid  pgsendfunc;         /* PgSQL convert type to binary */
	bool pgsendvarlena;

	/* OGR metadata */
	OgrColumnVariant ogrvariant;
	int ogrfldnum;
	OGRFieldType ogrfldtype;
} OgrFdwColumn;

typedef struct OgrFdwTable
{
	int ncols;
	char* tblname;
	OgrFdwColumn* cols;
} OgrFdwTable;

typedef struct OgrFdwSpatialFilter
{
	int ogrfldnum;
	double minx, miny, maxx, maxy;
} OgrFdwSpatialFilter;

typedef struct OgrConnection
{
	const char* ds_str;         /* datasource connection string */
	const char* dr_str;         /* driver (format) name */
	char* lyr_str;              /* layer name */
	const char* config_options; /* GDAL config options */
	const char* open_options;   /* GDAL open options */
	OgrUpdateable ds_updateable;
	OgrUpdateable lyr_updateable;
	int char_encoding;   /* Is OGR layer UTF? Has user provided encoding open option? */
	GDALDatasetH ds;      /* GDAL datasource handle */
	OGRLayerH lyr;        /* OGR layer handle */
} OgrConnection;

typedef enum
{
	OGR_PLAN_STATE,
	OGR_EXEC_STATE,
	OGR_MODIFY_STATE
} OgrFdwStateType;

typedef struct OgrFdwState
{
	OgrFdwStateType type;
	Oid foreigntableid;
	OgrConnection ogr;   /* connection object */
	OgrFdwTable* table;
	TupleDesc tupdesc;
} OgrFdwState;

typedef struct OgrFdwPlanState
{
	OgrFdwStateType type;
	Oid foreigntableid;
	OgrConnection ogr;
	OgrFdwTable* table;
	TupleDesc tupdesc;
	int nrows;           /* estimate of number of rows in file */
	Cost startup_cost;
	Cost total_cost;
	bool* pushdown_clauses;
} OgrFdwPlanState;

typedef struct OgrFdwExecState
{
	OgrFdwStateType type;
	Oid foreigntableid;
	OgrConnection ogr;
	OgrFdwTable *table;
	TupleDesc tupdesc;
	char* sql;              /* OGR SQL for attribute filter */
	int rownum;             /* how many rows have we read thus far? */
	Oid setsridfunc;        /* ST_SetSRID() */
	Oid typmodsridfunc;     /* postgis_typmod_srid() */
} OgrFdwExecState;

typedef struct OgrFdwModifyState
{
	OgrFdwStateType type;
	Oid foreigntableid;
	OgrConnection ogr;     /* connection object */
	OgrFdwTable* table;
	TupleDesc tupdesc;
} OgrFdwModifyState;

/* Shared function signatures */
bool ogrDeparse(StringInfo buf, PlannerInfo* root, RelOptInfo* foreignrel, List* exprs, OgrFdwState* state, List** params_list, OgrFdwSpatialFilter** sf);
Oid ogrGetGeometryOid(void);
OGRErr pgDatumToOgrGeometry (Datum pg_geometry, Oid pgsendfunc, OGRGeometryH* ogr_geometry);

#endif /* _OGR_FDW_H */
