* Abstract

  gfs_pio_open() ����� gfs_pio_create() �θ塢gfmd ���ե����륪���Ф�����硢
  �ե����륢�������Ϥδؿ���¹Ԥ���ȡ��ե����륪���и���³���ƥե����륢
  �������Ǥ��뤳�Ȥ��ǧ���롣

* Condition

  - gfmd ����Ĺ�������Ǥ��롣
  - ��˻��Ѥ��� gfarm�桼���� /tmp ���Ф��ƽ񤭹��߸��¤���ġ�

* Set up / Clean up

  - ���: setup.sh ��¹Ԥ��롣
  - ���: cleanup.sh ��¹Ԥ��롣

* Test Item

  �ƥ��ȼ¹ԥ��ޥ�ɤ��ȤΥƥ����оݰ���

  test-sched-read.sh         ... scheduling��, gfs_pio_read()

  test-sched-create-write.sh ... scheduling��,
                                 gfs_pio_create(), gfs_pio_write()

  test-sched-open-write.sh   ... scheduling��,
                                 gfs_pio_open(), gfs_pio_write()

  test-read.sh               ... scheduling��/��, gfs_pio_read()

  test-read-stat.sh          ... scheduling��/��,
                                 gfs_pio_read(), gfs_pio_stat()

  test-getc.sh               ... scheduling��/��,
                                 gfs_pio_getc(), gfs_pio_ungetc()

  test-seek.sh               ... scheduling��/��, buffer dirty�ǤϤʤ�����,
                                 gfs_pio_seek()

  test-seek-dirty.sh         ... scheduling��/��, buffer dirty�ξ���,
                                 gfs_pio_seek()

  test-write.sh              ... scheduling��/��, gfs_pio_write()

  test-write-stat.sh         ... scheduling��/��, gfs_pio_write(), gfs_pio_stat()

  test-putc.sh               ... scheduling��/��, gfs_pio_putc()

  test-truncate.sh           ... scheduling�� buffer dirty�ǤϤʤ�����
                                 scheduling�� buffer dirty�ξ���,
								 gfs_pio_truncate()

  test-flush.sh              ... scheduling�� buffer dirty�ǤϤʤ�����
                                 scheduling�� buffer dirty�ξ���,
								 gfs_pio_flush()

  test-sync.sh               ... scheduling�� buffer dirty�ǤϤʤ�����
                                 scheduling�� buffer dirty�ξ���,
								 gfs_pio_sync()

  test-datasync.sh           ... scheduling�� buffer dirty�ǤϤʤ�����
                                 scheduling�� buffer dirty�ξ���,
								 gfs_pio_datasync()

* Procedure

  ���Υ��ޥ�ɤ�¹Ԥ��롣

  $ ./test-all.sh

  �ƥ��ޥ�ɼ¹����

    *** wait for SIGUSR2 to continue ***

  ��ɽ�����줿�顢�̤Υ����뤫�� root �桼���Ǽ��Υ��ޥ�ɤ�¹Ԥ���
  gfmd ��ե����륪���Ф��롣

  (master gfmd �Υۥ��Ȥˤ�����)
  # ./gfmd-kill.sh 
  (slave gfmd �Υۥ��Ȥˤ�����)
  # ./gfmd-tomaster.sh

  ³���Ƽ��Υ��ޥ�ɤǥ��ޥ�ɤμ¹Ԥ��³���롣
  $ ./resume.sh

  ���ޥ�ɤκǸ�� "OK" �����Ϥ��줿������������ʳ��ξ��ϼ��ԡ�

  << ���� >>

  1. test-all.sh �Τ����ˡ�test-all.sh �Ǽ¹Ԥ���� test-*.sh ����̤�
     �¹Ԥ��Ƥ��ɤ���

  2. test-all.sh �˴Ķ��ѿ� SLEEP ���Ϥ��ȡ��ƥƥ��ȼ¹Ը�� SLEEP �ô֤���
     ��ߤ��롣����ϥե����륪���и�� gfsd �� gfmd ����³����ޤ��Ե�����
	 ������̤����롣

