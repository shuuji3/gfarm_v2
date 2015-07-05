#!/bin/sh

# this test may fail, if number of files which don't have enough
# replicas are too many.

NCOPY_TIMEOUT=60  # sec.

. ./regress.conf
tmpf=$gftmp/foo
base=`dirname $0`
. ${base}/replica_check-common.sh

check_supported_env
trap 'clean_test; exit $exit_trap' $trap_sigs
clean_test
setup_test_ncopy

set_ncopy $NCOPY1 $gftmp
wait_for_rep $NCOPY1 $tmpf false

set_ncopy $NCOPY2 $gftmp
wait_for_rep $NCOPY2 $tmpf false

clean_test

exit $exit_code
