#!/bin/sh

# this test may fail, if number of files which don't have enough
# replicas are too many.

. ./regress.conf
tmpf=$gftmp/foo
base=`dirname $0`
. ${base}/replica_check-common.sh

check_supported_env
backup_hostgroup
trap 'restore_hostgroup; clean_test; exit $exit_trap' $trap_sigs
clean_test
setup_test_repattr

set_repattr $NCOPY1 $gftmp
wait_for_rep $NCOPY1 $tmpf false

set_repattr $NCOPY2 $gftmp
wait_for_rep $NCOPY2 $tmpf false

restore_hostgroup
clean_test

exit $exit_code
