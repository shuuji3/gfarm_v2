* Abstract

  gfs.h/gfs_misc.h �θ����ؿ�����Ӹ����ؿ��ι������ǤȤʤ������ؿ��Τ�����
  gfmd �ؤΥ���������ޤ�ؿ��ˤĤ��ơ� gfmd ���ե����륪���Ф�����Ǥ��³
  ���Ƽ¹ԤǤ��뤳�Ȥ��ǧ���롣

* Condition

  - gfmd ����Ĺ�������Ǥ��롣
  - gfsd �� 2 �İʾ幽������Ƥ��롣
  - ��˻��Ѥ��� gfarm �桼���� /tmp ���Ф��ƽ񤭹��߸��¤���ġ�

* Set up / Clean up

  - ��ư�ƥ���(test-all.sh auto)��¹Ԥ�����ϡ�gfmd-failover-local.sh ��
    gfmd ��ե����륪���Ф��륹����ץȤ򵭽Ҥ��뤳�ȡ�

    # ����Ū�� gfmd �ե����륪���Ф��륹����ץȤϴĶ���¸�Ǥ��뤿�ᡢ
    # �ƥ��ȥ������ˤϴޤޤ�Ƥ��ʤ����ƥ��ȴĶ����Ȥ˵��Ҥ��ʤ���Фʤ�ʤ���

    �ʰ�Ū�ʥƥ��Ȥ�Ԥ��ˤ� gfmd-failover-local.sample.sh �����Ƥ򻲹ͤ�
    ���ơ�������� gfmd ��Ƶ�ư���륹����ץȤ򵭽Ҥ�����ɤ���
    gfmd ��Ƶ�ư���뤳�Ȥǡ��ե����륪���Ф˶ᤤ���̤����뤳�Ȥ��Ǥ��롣

    gfmd �ե����륪���Ф�ή��ϼ��ΤȤ��ꡣ

    1. master gfmd �����
    2. slave gfmd ���Ф��� SIGUSR1 ��ȯ�ԡ�

    �ʾ�ν��������Ǥϰ��٤Υե����륪���Фǽ���äƤ��ޤ����ᡢ�ƥ��ȹ��ܤ�Ϣ³
    �¹Ԥ��뤿��ˤϡ�1. �����˸� master gfmd �� slave �Ȥ��Ƶ�ư���ʤ����
    �ʤ�ʤ���

    ��ư���ϡ���ư��λ�ޤǤδ֤˥��饤�����(�ƥ��ȥ�����ץ�)��
    �����������Ƥ��Ƥ⡢���饤�����¦�� connection resuse ���Τ���
    ��ȥ饤����Τǡ�sleep���������ɬ�פϤʤ���

  - �ƥ����ѥǡ����� setup/cleanup �� test-all.sh ��Ǽ¹Ԥ���뤿�ᡢ
    test-all.sh��¹Ԥ���ݤ� �ƤӽФ�ɬ�פϤʤ���

  - ���̤˼�ư�Ǽ¹Ԥ�����ϡ��ʲ��Υ�����ץȤ�¹Ԥ��뤳�ȡ�
    - ���: setup.sh ��¹Ԥ��롣
    - ���: cleanup.sh ��¹Ԥ��롣

* Procedure - ��ư�ƥ���

  ���Υ��ޥ�ɤ�¹Ԥ��롣

  $ ./test-all.sh auto

* Procedure - ��ư�ƥ���

  ���Υ��ޥ�ɤ�¹Ԥ��롣

  $ ./test-all.sh

  �ƥ��ޥ�ɼ¹����

    *** Push Enter Key ***

  ��ɽ�����줿�顢�̤Υ�����Ǽ�ư�� gfmd ��ե����륪���Ф��롣
  ³���Ƹ��Υ������ Enter Key �����Ϥ���ȥƥ��Ȥ���³���롣

* Result

  - failed-list

    ���Ԥ����ƥ��ȼ��̤ϥե����� failed-list ����¸����롣

  - log

    �ƥ��ȥץ����ν��Ϥ� log ����¸����롣(��ư�ƥ��Ȼ���stdout)

* Note

  - �ƥ��ȼ��̤���ꤷ�Ƹ��̤˼¹Ԥ���ˤϡ�
    test-all.sh �Τ����ˡ�"test-launch.sh [�ƥ��ȼ���]" ��¹Ԥ��롣

  - �ƥ��Ȥ��ɲä���Ȥ��ϡ�test-list �˿������ƥ��ȼ��̤��ɲä��롣

  - test-all.sh �˴Ķ��ѿ� SLEEP ���Ϥ��ȡ��ƥƥ��ȼ¹Ը�� SLEEP �ô֤���
    ��ߤ��롣����ϥե����륪���и�� gfsd �� gfmd ����³����ޤ��Ե�����
    ������̤����뤬��gfm_client_connect() �Υ�ȥ饤�������������줿����
    �ˤ�����פˤʤä���

* Test Items

  test-launch.sh �ΰ������Ϥ��ƥ��ȼ��̤��ȤΥƥ����оݰ�����

  gfmd�إ���������������ؿ��Τ����������ǰʲ��Υƥ����оݴؿ��ˤ�äƤ���
  gfmd�إ����������Ƥ����Ρ��ޤ��ϥƥ����оݴؿ�������ʬ�Υ��å���ͭ
  ���Ƥ����ΤˤĤ��Ƥϡ��ƥ����оݤ���������Ƥ��롣
  �������줿�����ؿ��ˤĤ��Ƥ� gfs_pio_failover_test.c �� test_infos �������
  �˵��Ҥ��줿�����Ȥ򻲾ȤΤ��ȡ�

 - basic

  realpath           ... gfs_realpath()
  rename             ... gfs_rename()
  statfs             ... gfs_statfs()
  statfsnode         ... gfs_statfsnode()
  chmod              ... gfs_chmod()
  lchmod             ... gfs_lchmod()
  chown              ... gfs_chown()
  lchown             ... gfs_lchown()
  readlink           ... gfs_readlink()
  stat               ... gfs_stat()
  lstat              ... gfs_lstat()
  fstat              ... gfs_fstat()
  utimes             ... gfs_utimes()
  lutimes            ... gfs_lutimes()
  remove             ... gfs_remove()
  unlink             ... gfs_unlink()
  link               ... gfs_link()
  symlink            ... gfs_symlink()
  mkdir              ... gfs_rmdir()
  opendir            ... gfs_opendir()
  opendirplus        ... gfs_opendirplus()
  opendirplusxattr   ... gfs_opendirplusxattr() *1
  closedir           ... gfs_closedir()
  closedirplus       ... gfs_closedirplus() *1
  closedirplusxattr  ... gfs_closedirplusxattr()
  readdir            ... gfs_readdir()
  readdir2           ... gfs_readdir(), ¾�����ˤ�ꤹ�Ǥ�failover�������Ԥ�
                         ��Ƥ��ơ�GFS_Dir���Ť�gfm_connection����ͭ���Ƥ���
                         ������
  readdirplus        ... gfs_readdirplus()
  readdirplusxattr   ... gfs_readdirplusxattr() *1
  seekdir            ... gfs_seekdir()
  seekdirplusxattr   ... gfs_seekdirplusxattr() *1

  *1 gfs_*dirxattrplus() �ϸ����ؿ��ǤϤʤ�����gfs_opendir_caching() ��
     �̤��� gfarm2fs ����ƤФ�뤿��ƥ����оݤȤ��Ƥ��롣

 - gfs_pio

  sched-read         ... scheduling��, gfs_pio_read()
  sched-create-write ... scheduling��, gfs_pio_create(), gfs_pio_write()
  sched-open-write   ... scheduling��, gfs_pio_open(), gfs_pio_write()
  close              ... scheduling��, gfs_pio_close()
  close-open         ... gfm_connection error���close�ˤ��
                         ʣ����GFS_File�˰ۤʤ�gfm_connection�����ꤵ��륱����,
                         scheduling�夬����
  close-open2        ... gfm_connection error���close�ˤ��
                         ʣ����GFS_File�˰ۤʤ�gfm_connection�����ꤵ��륱����,
                         scheduling��
  read               ... scheduling��/��, gfs_pio_read()
  read-stat          ... scheduling��/��, gfs_pio_read(), gfs_pio_stat()
  getc               ... scheduling��/��, gfs_pio_getc(), gfs_pio_ungetc()
  seek               ... scheduling��/��, buffer dirty�ǤϤʤ�����,
                         gfs_pio_seek()
  seek-dirty         ... scheduling��/��, buffer dirty�ξ���, gfs_pio_seek()
  write              ... scheduling��/��, gfs_pio_write()
  write-stat         ... scheduling��/��, gfs_pio_write(), gfs_pio_stat()
  putc               ... scheduling��/��, gfs_pio_putc()
  truncate           ... scheduling�� buffer dirty�ǤϤʤ�����,
                         scheduling�� buffer dirty�ξ���, gfs_pio_truncate()
  flush              ... scheduling�� buffer dirty�ǤϤʤ�����
                         scheduling�� buffer dirty�ξ���, gfs_pio_flush()
  sync               ... scheduling�� buffer dirty�ǤϤʤ�����
                         scheduling�� buffer dirty�ξ���, gfs_pio_sync()
  datasync           ... scheduling�� buffer dirty�ǤϤʤ�����
                         scheduling�� buffer dirty�ξ���, gfs_pio_datasync()
 - xattr/xmlattr

  fsetxattr          ... gfs_fsetxattr()
  getxattr           ... gfs_getxattr()
  lgetxattr          ... gfs_lgetxattr()
  getattrplus        ... gfs_getattrplus()
  lgetattrplus       ... gfs_lgetattrplus()
  setxattr           ... gfs_setxattr()
  lsetxattr          ... gfs_lsetxattr()
  removexattr        ... gfs_removexattr()
  lremovexattr       ... gfs_lremovexattr()
  fgetxattr          ... gfs_fgetxattr()
  fsetxattr          ... gfs_fsetxattr()
  fremovexattr       ... gfs_fremovexattr()
  listxattr          ... gfs_listxattr()
  llistxattr         ... gfs_llistxattr()
  setxmlattr         ... gfs_setxmlattr()
  lsetxmlattr        ... gfs_lsetxmlattr()
  getxmlattr         ... gfs_getxmlattr()
  lgetxmlattr        ... gfs_lgetxmlattr()
  listxmlattr        ... gfs_listxmlattr()
  llistxmlattr       ... gfs_llistxmlattr()
  removexmlattr      ... gfs_removexmlattr()
  lremovexmlattr     ... gfs_lremovexmlattr()
  findxmlattr        ... gfs_findxmlattr()
  getxmlent          ... gfs_getxmlent()
  closexmlattr       ... gfs_closexmlattr()

 - scheduling

  shhosts            ... gfarm_schedule_hosts()
  shhosts-domainall  ... gfarm_schedule_hosts_domain_all()
  shhosts-domainfile ... gfarm_schedule_hosts_domain_by_file()

 - replication

  rep-info           ... gfs_replica_info_by_name()
  rep-list           ... gfs_replica_list_by_name()
  rep-to             ... gfs_replicate_to()
  rep-fromto         ... gfs_replicate_from_to()
  rep-toreq          ... gfs_replicate_file_to_request()
  rep-fromtoreq      ... gfs_replicate_file_from_to_request()
  rep-remove         ... gfs_replica_remove_by_file()
  migrate-to         ... gfs_migrate_to()
  migrate-fromto     ... gfs_migrate_fromto()

