#! /bin/sh

. ./regress.conf
exec $testbase/test_gfjournal.sh $testbase/two_transactions2.gmj
