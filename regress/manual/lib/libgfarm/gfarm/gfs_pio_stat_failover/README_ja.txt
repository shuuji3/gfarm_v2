* Abstract

  gfmd ���ե����륪���Ф����塢gfs_pio_stat() ���¹Ԥ����Ȥ��٤ƤΥ����ץ���
  �Υե�����˴ؤ��ƺ���³�������Ԥ�졢�ե����륪���и��ե�����إ���������
  ���뤳�Ȥ��ǧ���롣

* Condition

  - gfmd ����Ĺ�������Ǥ��롣
  - ��˻��Ѥ��� gfarm�桼���� /tmp ���Ф��ƽ񤭹��߸��¤���ġ�

* Set up / Clean up

  - ���: setup.sh ��¹Ԥ��롣
  - ���: clean.sh ��¹Ԥ��롣

* Test Item

  test-1.sh ... gfs_pio_read()
  test-2.sh ... gfs_pio_write()

* Procedure

  ���Υ��ޥ�ɤ�¹Ԥ��롣

  $ ./test-1.sh
  $ ./test-2.sh

  ���ޥ�ɼ¹����

   *** Please failover gfmd manually and push Enter Key ***

  ��ɽ�����줿�顢�̤Υ����뤫�� root �桼���Ǽ��Υ��ޥ�ɤ�¹Ԥ���
  gfmd ��ե����륪���Ф��Ƥ���������

  (master gfmd �Υۥ��Ȥˤ�����)
  # ./gfmd-kill.sh 
  (slave gfmd �Υۥ��Ȥˤ�����)
  # ./gfmd-tomaster.sh

  ���˸��Υ������Enter���������Ϥ��ơ��ƥ��Ȥ�³�Ԥ��Ƥ���������
  ���ޥ�ɤκǸ�� "OK" �����Ϥ��줿������������ʳ��ξ��ϼ��ԡ�

