###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-

import sys
from util.log import *
from util.cases import *
from util.sql import *


class TDTestCase:
    def init(self, conn):
        tdLog.debug("start to execute %s" % __file__)
        tdSql.init(conn.cursor())

    def run(self):
        tdSql.prepare()

        boundary = 32
        for x in range(0, boundary):
            stb_name = "stb%d" % x

            tagSeq = "tag0 int"
            for y in range(1, x+1):
                tagSeq = tagSeq + ", tag%d int" % y

            tdLog.info("create table %s (ts timestamp, value int) tags (%s)" % (stb_name, tagSeq))
            tdSql.execute("create table %s (ts timestamp, value int) tags (%s)" % (stb_name, tagSeq))

        tdSql.query("show stables")
        tdSql.checkRows(boundary)

        stb_name = "stb%d" % (boundary+1)
        tagSeq = tagSeq + ", tag%d int" % (boundary+1)
        tdLog.info("create table %s (ts timestamp, value int) tags (%s)" % (stb_name, tagSeq))
        tdSql.error("create table %s (ts timestamp, value int) tags (%s)" % (stb_name, tagSeq))
        tdSql.query("show stables")
        tdSql.checkRows(boundary)

    def stop(self):
        tdSql.close()
        tdLog.success("%s successfully executed" % __file__)


tdCases.addWindows(__file__, TDTestCase())
tdCases.addLinux(__file__, TDTestCase())
