/*
 * Copyright (c) 2020 TAOS Data, Inc. <jhtao@taosdata.com>
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
#include "mndTelem.h"
#include "mndCluster.h"
#include "mndSync.h"
#include "thttp.h"
#include "tjson.h"

typedef struct {
  int64_t numOfDnode;
  int64_t numOfMnode;
  int64_t numOfVgroup;
  int64_t numOfDatabase;
  int64_t numOfSuperTable;
  int64_t numOfChildTable;
  int64_t numOfNormalTable;
  int64_t numOfColumn;
  int64_t totalPoints;
  int64_t totalStorage;
  int64_t compStorage;
} SMnodeStat;

static void mndGetStat(SMnode* pMnode, SMnodeStat* pStat) {
  memset(pStat, 0, sizeof(SMnodeStat));

  SSdb* pSdb = pMnode->pSdb;
  pStat->numOfDnode = sdbGetSize(pSdb, SDB_DNODE);
  pStat->numOfMnode = sdbGetSize(pSdb, SDB_MNODE);
  pStat->numOfVgroup = sdbGetSize(pSdb, SDB_VGROUP);
  pStat->numOfDatabase = sdbGetSize(pSdb, SDB_DB);
  pStat->numOfSuperTable = sdbGetSize(pSdb, SDB_STB);

  void* pIter = NULL;
  while (1) {
    SVgObj* pVgroup = NULL;
    pIter = sdbFetch(pSdb, SDB_VGROUP, pIter, (void**)&pVgroup);
    if (pIter == NULL) break;

    pStat->numOfChildTable += pVgroup->numOfTables;
    pStat->numOfColumn += pVgroup->numOfTimeSeries;
    pStat->totalPoints += pVgroup->pointsWritten;
    pStat->totalStorage += pVgroup->totalStorage;
    pStat->compStorage += pVgroup->compStorage;

    sdbRelease(pSdb, pVgroup);
  }

  pStat->numOfChildTable = 100;
  pStat->numOfColumn = 200;
  pStat->totalPoints = 300;
  pStat->totalStorage = 400;
  pStat->compStorage = 500;
}

static void mndBuildRuntimeInfo(SMnode* pMnode, SJson* pJson) {
  SMnodeStat mstat = {0};
  mndGetStat(pMnode, &mstat);

  (void)tjsonAddDoubleToObject(pJson, "numOfDnode", mstat.numOfDnode);
  (void)tjsonAddDoubleToObject(pJson, "numOfMnode", mstat.numOfMnode);
  (void)tjsonAddDoubleToObject(pJson, "numOfVgroup", mstat.numOfVgroup);
  (void)tjsonAddDoubleToObject(pJson, "numOfDatabase", mstat.numOfDatabase);
  (void)tjsonAddDoubleToObject(pJson, "numOfSuperTable", mstat.numOfSuperTable);
  (void)tjsonAddDoubleToObject(pJson, "numOfChildTable", mstat.numOfChildTable);
  (void)tjsonAddDoubleToObject(pJson, "numOfColumn", mstat.numOfColumn);
  (void)tjsonAddDoubleToObject(pJson, "numOfPoint", mstat.totalPoints);
  (void)tjsonAddDoubleToObject(pJson, "totalStorage", mstat.totalStorage);
  (void)tjsonAddDoubleToObject(pJson, "compStorage", mstat.compStorage);
}

static char* mndBuildTelemetryReport(SMnode* pMnode) {
  char        tmp[4096] = {0};
  STelemMgmt* pMgmt = &pMnode->telemMgmt;

  SJson* pJson = tjsonCreateObject();
  if (pJson == NULL) return NULL;

  char clusterName[64] = {0};
  if ((terrno = mndGetClusterName(pMnode, clusterName, sizeof(clusterName))) != 0) return NULL;
  (void)tjsonAddStringToObject(pJson, "instanceId", clusterName);
  (void)tjsonAddDoubleToObject(pJson, "reportVersion", 1);

  if (taosGetOsReleaseName(tmp, NULL, NULL, sizeof(tmp)) == 0) {
    (void)tjsonAddStringToObject(pJson, "os", tmp);
  }

  float numOfCores = 0;
  if (taosGetCpuInfo(tmp, sizeof(tmp), &numOfCores) == 0) {
    (void)tjsonAddStringToObject(pJson, "cpuModel", tmp);
    (void)tjsonAddDoubleToObject(pJson, "numOfCpu", numOfCores);
  } else {
    (void)tjsonAddDoubleToObject(pJson, "numOfCpu", tsNumOfCores);
  }

  snprintf(tmp, sizeof(tmp), "%" PRId64 " kB", tsTotalMemoryKB);
  (void)tjsonAddStringToObject(pJson, "memory", tmp);

  (void)tjsonAddStringToObject(pJson, "version", version);
  (void)tjsonAddStringToObject(pJson, "buildInfo", buildinfo);
  (void)tjsonAddStringToObject(pJson, "gitInfo", gitinfo);
  (void)tjsonAddStringToObject(pJson, "email", pMgmt->email);

  mndBuildRuntimeInfo(pMnode, pJson);

  char* pCont = tjsonToString(pJson);
  tjsonDelete(pJson);
  return pCont;
}

static int32_t mndProcessTelemTimer(SRpcMsg* pReq) {
  SMnode*     pMnode = pReq->info.node;
  STelemMgmt* pMgmt = &pMnode->telemMgmt;
  if (!tsEnableTelem) return 0;

  (void)taosThreadMutexLock(&pMgmt->lock);
  char* pCont = mndBuildTelemetryReport(pMnode);
  (void)taosThreadMutexUnlock(&pMgmt->lock);

  if (pCont != NULL) {
    if (taosSendHttpReport(tsTelemServer, tsTelemUri, tsTelemPort, pCont, strlen(pCont), HTTP_FLAT) != 0) {
      mError("failed to send telemetry report");
    } else {
      mInfo("succeed to send telemetry report");
    }
    taosMemoryFree(pCont);
  }
  return 0;
}

int32_t mndInitTelem(SMnode* pMnode) {
  STelemMgmt* pMgmt = &pMnode->telemMgmt;

  (void)taosThreadMutexInit(&pMgmt->lock, NULL);
  TAOS_CHECK_RETURN(taosGetEmail(pMgmt->email, sizeof(pMgmt->email)));
  mndSetMsgHandle(pMnode, TDMT_MND_TELEM_TIMER, mndProcessTelemTimer);

  return 0;
}

void mndCleanupTelem(SMnode* pMnode) {
  STelemMgmt* pMgmt = &pMnode->telemMgmt;
  (void)taosThreadMutexDestroy(&pMgmt->lock);
}
