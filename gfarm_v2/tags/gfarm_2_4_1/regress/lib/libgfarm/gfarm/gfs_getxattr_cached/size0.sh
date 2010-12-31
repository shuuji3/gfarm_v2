#!/bin/sh

. ./regress.conf

name=foo
value=hoge

trap 'gfrm -f $gftmp; exit $exit_trap' $trap_sigs

if gfreg $data/1byte $gftmp &&
   echo -n "$value" | gfxattr -s $gftmp $name &&
   $testbin/size0 $gftmp $name &&
   $testbin/size0 -c $gftmp $name
then
    exit_code=$exit_pass
fi

gfrm $gftmp
rm -f $localtmp
exit $exit_code
