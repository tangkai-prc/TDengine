/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"

#include "mnode.h"
#include "mgmtAcct.h"
#include "mgmtGrant.h"
#include "mgmtUtil.h"
#include "mgmtDb.h"
#include "mgmtDnodeInt.h"
#include "mgmtVgroup.h"
#include "mgmtTable.h"
#include "taosmsg.h"
#include "tast.h"
#include "textbuffer.h"
#include "tschemautil.h"
#include "tscompression.h"
#include "tskiplist.h"
#include "tsqlfunction.h"
#include "ttime.h"
#include "tstatus.h"

#include "sdb.h"

#include "mgmtChildTable.h"
#include "mgmtChildTable.h"



void *tsChildTableSdb;
void *(*mgmtChildTableActionFp[SDB_MAX_ACTION_TYPES])(void *row, char *str, int size, int *ssize);

void *mgmtChildTableActionInsert(void *row, char *str, int size, int *ssize);
void *mgmtChildTableActionDelete(void *row, char *str, int size, int *ssize);
void *mgmtChildTableActionUpdate(void *row, char *str, int size, int *ssize);
void *mgmtChildTableActionEncode(void *row, char *str, int size, int *ssize);
void *mgmtChildTableActionDecode(void *row, char *str, int size, int *ssize);
void *mgmtChildTableActionReset(void *row, char *str, int size, int *ssize);
void *mgmtChildTableActionDestroy(void *row, char *str, int size, int *ssize);

static void mgmtDestroyChildTable(SChildTableObj *pTable) {
  free(pTable);
}

static void mgmtChildTableActionInit() {
  mgmtChildTableActionFp[SDB_TYPE_INSERT] = mgmtChildTableActionInsert;
  mgmtChildTableActionFp[SDB_TYPE_DELETE] = mgmtChildTableActionDelete;
  mgmtChildTableActionFp[SDB_TYPE_UPDATE] = mgmtChildTableActionUpdate;
  mgmtChildTableActionFp[SDB_TYPE_ENCODE] = mgmtChildTableActionEncode;
  mgmtChildTableActionFp[SDB_TYPE_DECODE] = mgmtChildTableActionDecode;
  mgmtChildTableActionFp[SDB_TYPE_RESET] = mgmtChildTableActionReset;
  mgmtChildTableActionFp[SDB_TYPE_DESTROY] = mgmtChildTableActionDestroy;
}

void *mgmtChildTableActionReset(void *row, char *str, int size, int *ssize) {
  return NULL;
}

void *mgmtChildTableActionDestroy(void *row, char *str, int size, int *ssize) {
  SChildTableObj *pTable = (SChildTableObj *)row;
  mgmtDestroyChildTable(pTable);
  return NULL;
}

void *mgmtChildTableActionInsert(void *row, char *str, int size, int *ssize) {
  SChildTableObj *pTable = (SChildTableObj *) row;

  SVgObj *pVgroup = mgmtGetVgroup(pTable->vgId);
  if (pVgroup == NULL) {
    mError("id:%s not in vgroup:%d", pTable->tableId, pTable->vgId);
    return NULL;
  }

  SDbObj *pDb = mgmtGetDb(pVgroup->dbName);
  if (pDb == NULL) {
    mError("vgroup:%d not in DB:%s", pVgroup->vgId, pVgroup->dbName);
    return NULL;
  }

  SAcctObj *pAcct = mgmtGetAcct(pDb->cfg.acct);
  if (pAcct == NULL) {
    mError("account not exists");
    return NULL;
  }


  if (!sdbMaster) {
    int sid = taosAllocateId(pVgroup->idPool);
    if (sid != pTable->sid) {
      mError("sid:%d is not matched from the master:%d", sid, pTable->sid);
      return NULL;
    }
  }

  mgmtAddMeterIntoMetric(pTable->superTableId, pTable);

  pAcct->acctInfo.numOfTimeSeries += (pTable->superTable->numOfColumns - 1);
  pVgroup->numOfMeters++;
  pDb->numOfTables++;
  pVgroup->meterList[pTable->sid] = pTable;

  if (pVgroup->numOfMeters >= pDb->cfg.maxSessions - 1 && pDb->numOfVgroups > 1) {
    mgmtMoveVgroupToTail(pDb, pVgroup);
  }

  return NULL;
}

void *mgmtChildTableActionDelete(void *row, char *str, int size, int *ssize) {
  SChildTableObj *pTable = (SChildTableObj *) row;
  if (pTable->vgId == 0) {
    return NULL;
  }

  SVgObj *pVgroup = mgmtGetVgroup(pTable->vgId);
  if (pVgroup == NULL) {
    mError("id:%s not in vgroup:%d", pTable->tableId, pTable->vgId);
    return NULL;
  }

  SDbObj *pDb = mgmtGetDb(pVgroup->dbName);
  if (pDb == NULL) {
    mError("vgroup:%d not in DB:%s", pVgroup->vgId, pVgroup->dbName);
    return NULL;
  }

  SAcctObj *pAcct = mgmtGetAcct(pDb->cfg.acct);
  if (pAcct == NULL) {
    mError("account not exists");
    return NULL;
  }

  pAcct->acctInfo.numOfTimeSeries -= (pTable->superTable->numOfColumns - 1);
  pVgroup->meterList[pTable->sid] = NULL;
  pVgroup->numOfMeters--;
  pDb->numOfTables--;
  taosFreeId(pVgroup->idPool, pTable->sid);

  mgmtRemoveMeterFromMetric(pTable->superTable, pTable);

  if (pVgroup->numOfMeters > 0) {
    mgmtMoveVgroupToHead(pDb, pVgroup);
  }

  return NULL;
}

void *mgmtChildTableActionUpdate(void *row, char *str, int size, int *ssize) {
  return mgmtChildTableActionReset(row, str, size, NULL);
}

void *mgmtChildTableActionEncode(void *row, char *str, int size, int *ssize) {
  SChildTableObj *pTable = (SChildTableObj *) row;
  assert(row != NULL && str != NULL);

  int tsize = pTable->updateEnd - (int8_t *) pTable;
  memcpy(str, pTable, tsize);

  return NULL;
}

void *mgmtChildTableActionDecode(void *row, char *str, int size, int *ssize) {
  assert(str != NULL);

  SChildTableObj *pTable = (SChildTableObj *)malloc(sizeof(SChildTableObj));
  if (pTable == NULL) {
    return NULL;
  }
  memset(pTable, 0, sizeof(STabObj));

  int tsize = pTable->updateEnd - (int8_t *)pTable;
  if (size < tsize) {
    mgmtDestroyChildTable(pTable);
    return NULL;
  }
  memcpy(pTable, str, tsize);

  return (void *)pTable;
}

void *mgmtChildTableAction(char action, void *row, char *str, int size, int *ssize) {
  if (mgmtChildTableActionFp[(uint8_t)action] != NULL) {
    return (*(mgmtChildTableActionFp[(uint8_t)action]))(row, str, size, ssize);
  }
  return NULL;
}

int32_t mgmtInitChildTables() {
  return 0;
}

void mgmtCleanUpChildTables() {
}

int8_t *mgmtBuildCreateChildTableMsg(SChildTableObj *pTable, int8_t *pMsg, int32_t vnode, int32_t tagDataLen,
                                     int8_t *pTagData) {
  SCreateTableMsg *pCreateTable = (SCreateTableMsg *) pMsg;
//  memcpy(pCreateTable->tableId, pTable->tableId, TSDB_TABLE_ID_LEN);
//  memcpy(pCreateTable->superTableId, pTable->superTable->tableId, TSDB_TABLE_ID_LEN);
//  pCreateTable->vnode        = htonl(vnode);
//  pCreateTable->sid          = htonl(pTable->sid);
//  pCreateTable->uid          = pTable->uid;
//  pCreateTable->createdTime  = htobe64(pTable->createdTime);
//  pCreateTable->sversion     = htonl(pTable->superTable->sversion);
//  pCreateTable->numOfColumns = htons(pTable->superTable->numOfColumns);
//  pCreateTable->numOfTags    = htons(pTable->superTable->numOfTags);
//
//  SSchema *pSchema  = pTable->superTable->schema;
//  int32_t totalCols = pCreateTable->numOfColumns + pCreateTable->numOfTags;
//
//  for (int32_t col = 0; col < totalCols; ++col) {
//    SMColumn *colData = &((SMColumn *) (pCreateTable->data))[col];
//    colData->type  = pSchema[col].type;
//    colData->bytes = htons(pSchema[col].bytes);
//    colData->colId = htons(pSchema[col].colId);
//  }
//
//  int32_t totalColsSize = sizeof(SMColumn *) * totalCols;
//  pMsg = pCreateTable->data + totalColsSize + tagDataLen;
//
//  memcpy(pCreateTable->data + totalColsSize, pTagData, tagDataLen);
//  pCreateTable->tagDataLen = htonl(tagDataLen);

  return pMsg;
}

int32_t mgmtCreateChildTable(SDbObj *pDb, SCreateTableMsg *pCreate, SVgObj *pVgroup, int32_t sid) {
//  int numOfTables = sdbGetNumOfRows(tsChildTableSdb);
//  if (numOfTables >= tsMaxTables) {
//    mError("table:%s, numOfTables:%d exceed maxTables:%d", pCreate->meterId, numOfTables, tsMaxTables);
//    return TSDB_CODE_TOO_MANY_TABLES;
//  }
//
//  char           *pTagData    = (char *) pCreate->schema;  // it is a tag key
//  SSuperTableObj *pSuperTable = mgmtGetSuperTable(pTagData);
//  if (pSuperTable == NULL) {
//    mError("table:%s, corresponding super table does not exist", pCreate->meterId);
//    return TSDB_CODE_INVALID_TABLE;
//  }
//
//  SChildTableObj *pTable = (SChildTableObj *) calloc(sizeof(SChildTableObj), 1);
//  if (pTable == NULL) {
//    return TSDB_CODE_SERV_OUT_OF_MEMORY;
//  }
//  strcpy(pTable->tableId, pCreate->meterId);
//  strcpy(pTable->superTableId, pSuperTable->tableId);
//  pTable->createdTime = taosGetTimestampMs();
//  pTable->superTable  = pSuperTable;
//  pTable->vgId        = pVgroup->vgId;
//  pTable->sid         = sid;
//  pTable->uid         = (((uint64_t) pTable->vgId) << 40) + ((((uint64_t) pTable->sid) & ((1ul << 24) - 1ul)) << 16) +
//                        ((uint64_t) sdbGetVersion() & ((1ul << 16) - 1ul));
//
//  SVariableMsg tags = {0};
//  tags.size = mgmtGetTagsLength(pSuperTable, INT_MAX) + (uint32_t) TSDB_TABLE_ID_LEN;
//  tags.data = (char *) calloc(1, tags.size);
//  if (tags.data == NULL) {
//    free(pTable);
//    mError("table:%s, corresponding super table schema is null", pCreate->meterId);
//    return TSDB_CODE_INVALID_TABLE;
//  }
//  memcpy(tags.data, pTagData, tags.size);
//
//  if (sdbInsertRow(tsStreamTableSdb, pTable, 0) < 0) {
//    mError("table:%s, update sdb error", pCreate->meterId);
//    return TSDB_CODE_SDB_ERROR;
//  }
//
//  mgmtAddTimeSeries(pTable->superTable->numOfColumns - 1);
//
//  mgmtSendCreateChildTableMsg(pTable, pVgroup, tags.size, tags.data);
//
//  mTrace("table:%s, create table in vgroup, vgId:%d sid:%d vnode:%d uid:%"
//             PRIu64
//             " db:%s",
//         pTable->tableId, pVgroup->vgId, sid, pVgroup->vnodeGid[0].vnode, pTable->uid, pDb->name);

  return 0;
}

int32_t mgmtDropChildTable(SDbObj *pDb, SChildTableObj *pTable) {
  SVgObj *pVgroup;
  SAcctObj *pAcct;

  pAcct = mgmtGetAcct(pDb->cfg.acct);

  if (pAcct != NULL) {
    pAcct->acctInfo.numOfTimeSeries -= (pTable->superTable->numOfColumns - 1);
  }

  pVgroup = mgmtGetVgroup(pTable->vgId);
  if (pVgroup == NULL) {
    return TSDB_CODE_OTHERS;
  }

  mgmtRestoreTimeSeries(pTable->superTable->numOfColumns - 1);
  mgmtSendRemoveMeterMsgToDnode(pTable, pVgroup);
  sdbDeleteRow(tsChildTableSdb, pTable);

  if (pVgroup->numOfMeters <= 0) {
    mgmtDropVgroup(pDb, pVgroup);
  }

  return 0;
}

SChildTableObj* mgmtGetChildTable(char *tableId) {
  return (SChildTableObj *)sdbGetRow(tsChildTableSdb, tableId);
}

int32_t mgmtModifyChildTableTagValueByName(SChildTableObj *pTable, char *tagName, char *nContent) {
//  int col = mgmtFindTagCol(pTable->superTable, tagName);
//  if (col < 0 || col > pTable->superTable->numOfTags) {
//    return TSDB_CODE_APP_ERROR;
//  }
//
//  //TODO send msg to dnode
//  mTrace("Succeed to modify tag column %d of table %s", col, pTable->tableId);
//  return TSDB_CODE_SUCCESS;

//  int rowSize = 0;
//  SSchema *schema = (SSchema *)(pSuperTable->schema + (pSuperTable->numOfColumns + col) * sizeof(SSchema));
//
//  if (col == 0) {
//    pTable->isDirty = 1;
//    removeMeterFromMetricIndex(pSuperTable, pTable);
//  }
//  memcpy(pTable->pTagData + mgmtGetTagsLength(pMetric, col) + TSDB_TABLE_ID_LEN, nContent, schema->bytes);
//  if (col == 0) {
//    addMeterIntoMetricIndex(pMetric, pTable);
//  }
//
//  // Encode the string
//  int   size = sizeof(STabObj) + TSDB_MAX_BYTES_PER_ROW + 1;
//  char *msg = (char *)malloc(size);
//  if (msg == NULL) {
//    mError("failed to allocate message memory while modify tag value");
//    return TSDB_CODE_APP_ERROR;
//  }
//  memset(msg, 0, size);
//
//  mgmtMeterActionEncode(pTable, msg, size, &rowSize);
//
//  int32_t ret = sdbUpdateRow(meterSdb, msg, rowSize, 1);  // Need callback function
//  tfree(msg);
//
//  if (pTable->isDirty) pTable->isDirty = 0;
//
//  if (ret < 0) {
//    mError("Failed to modify tag column %d of table %s", col, pTable->meterId);
//    return TSDB_CODE_APP_ERROR;
//  }
//
//  mTrace("Succeed to modify tag column %d of table %s", col, pTable->meterId);
//  return TSDB_CODE_SUCCESS;
}

