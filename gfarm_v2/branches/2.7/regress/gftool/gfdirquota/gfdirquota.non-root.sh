#!/bin/sh

. ./regress.conf

if $regress/bin/am_I_gfarmroot; then
	exit $exit_unsupported
fi

dirset=`hostname`.regress.set-$$

trap 'gfrm -rf ${gftmp}.0 ${gftmp}.A ${gftmp}.A2 ${gftmp}.B;
	gfdirquota -d ${dirset}.A ${dirset}.B;
	exit $exit_trap' $trap_sigs

if gfmkdir ${gftmp}.0 &&
   gfmkdir ${gftmp}.A &&
   gfmkdir ${gftmp}.B &&
   gfmkdir ${gftmp}.A2 &&
   gfdirquota -c ${dirset}.A &&
   gfdirquota -c ${dirset}.B &&
   gfdirquota -a ${dirset}.A ${gftmp}.A &&
   gfdirquota -a ${dirset}.A ${gftmp}.A2 &&
   gfreg $data/1byte ${gftmp}.0/file &&
   gfreg $data/1byte ${gftmp}.A/file &&
   gfreg $data/1byte ${gftmp}.B/file &&

   # only empty directory can be registered by a normal user
   gfdirquota -a ${dirset}.B ${gftmp}.B 2>&1 |
	grep 'directory not empty' >/dev/null &&

   gfrm ${gftmp}.B/file &&
   gfdirquota -a ${dirset}.B ${gftmp}.B &&

   # new hardlink cannot be created across a dirset
   gfln ${gftmp}.0/file ${gftmp}.A/file2 2>&1 |
	grep 'cross device link' >/dev/null &&
   gfln ${gftmp}.A/file ${gftmp}.0/file2 2>&1 |
	grep 'cross device link' >/dev/null &&
   gfln ${gftmp}.A/file ${gftmp}.B/file2 2>&1 |
	grep 'cross device link' >/dev/null &&

   # new hardlink between same dirset
   gfln ${gftmp}.0/file ${gftmp}.0/file2 &&
   gfln ${gftmp}.A/file ${gftmp}.A2/file2 &&

   # an existing hardlink cannot go across a different dirset
   gfmv ${gftmp}.0/file ${gftmp}.A/file3 2>&1 |
	grep 'operation not supported' >/dev/null
   gfmv ${gftmp}.A/file ${gftmp}.0/file3 2>&1 |
	grep 'operation not supported' >/dev/null
   gfmv ${gftmp}.A/file ${gftmp}.B/file3 2>&1 |
	grep 'operation not supported' >/dev/null

   # an existing hardlink can go to same dirset
   gfln ${gftmp}.A/file ${gftmp}.A2/file3 &&

   gfrm ${gftmp}.0/file2 &&
   gfrm ${gftmp}.A2/file2 ${gftmp}.A2/file3 &&

   # a non-hardlinked file can go across a different dirset
   gfmv ${gftmp}.0/file  ${gftmp}.A/file4 &&
   gfmv ${gftmp}.A/file4 ${gftmp}.B/file4 &&
   gfmv ${gftmp}.B/file4 ${gftmp}.0/file &&

   gfmkdir ${gftmp}.0/dir &&
   gfmkdir ${gftmp}.A/dir &&

   # subtree cannot be a different difset
   gfdirquota -a ${dirset}.B ${gftmp}.A/dir 2>&1 |
	grep 'already exists' >/dev/null &&

   # a directory can go same dirset
   gfmv ${gftmp}.A/dir ${gftmp}.A2/dir  &&

   # a directory cannot go across a different dirset
   gfmv ${gftmp}.0/dir  ${gftmp}.A/dir2 2>&1 |
	grep 'operation not supported' >/dev/null &&
   gfmv ${gftmp}.A2/dir ${gftmp}.0/dir2 2>&1 |
	grep 'operation not supported' >/dev/null &&
   gfmv ${gftmp}.A2/dir ${gftmp}.B/dir2 2>&1 |
	grep 'operation not supported' >/dev/null &&

   gfrm ${gftmp}.0/file ${gftmp}.A/file &&
   gfrmdir ${gftmp}.0/dir ${gftmp}.A2/dir \
	${gftmp}.0 ${gftmp}.A ${gftmp}.B ${gftmp}.A2
then
	exit_code=$exit_pass
fi

gfrm -rf ${gftmp}.0 ${gftmp}.A ${gftmp}.A2 ${gftmp}.B
gfdirquota -d ${dirset}.A ${dirset}.B
exit $exit_code
