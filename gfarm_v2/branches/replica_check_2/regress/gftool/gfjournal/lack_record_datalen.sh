#! /bin/sh

. ./regress.conf
exec $testbase/test_gfjournal.sh $testbase/lack_record_datalen.gmj
