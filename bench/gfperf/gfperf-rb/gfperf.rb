#!/usr/bin/env ruby
require 'fileutils'
require 'optparse'
require 'yaml'
require 'pp'
require 'open3'
require 'sqlite3'
require 'thread'

$error_message_lock = Mutex.new

def calc_pasttime(str)
  u = {"sec" => 1, "second" => 1, "secs" => 1, "seconds" => 1,
    "min" => 60, "minute" => 60, "mins" => 60, "minutes" => 60,
    "hour" => 60*60, "hours" => 60*60,
    "day" => 24*60*60, "days" => 24*60*60,
    "week" => 7*24*60*60, "weeks" => 7*24*60*60,
    "month" => 30*24*60*60, "months" => 30*24*60*60}

  a = str.scan(/(^[\d]*)[\s]*([a-zA-Z]*$)/)[0]
  num = a[0].to_i
  unit = u[a[1]].to_i
  return num * unit
end

def calc_stddev()
  sql = "select key from data group by key;"
  keys = $db.execute(sql)
  start = ($now - calc_pasttime($config["database"]["check span"])).to_i
  $db.execute("begin;")
  keys.each {|key|
    sql = "select val from data where key='#{key[0]}' and date > '#{start}';";
    vals = $db.execute(sql)
    if (vals.size > 0) 
      avr = vals.inject(0.0){|r,i| r+=i[0].to_f }/vals.size
      variance = vals.inject(0.0){|r,i| r+=(i[0].to_f-avr)**2 }/vals.size
      stddev = Math.sqrt(variance)
      sql="insert into statistics(date,key,avr,stddev) "+
        "values('#{$now.to_i}','#{key[0]}',#{avr},#{stddev});"
      $db.execute(sql)
    end
  }
  $db.execute("commit;")
end

def open_db()
  fn = $config["database"]["filename"]
  if (File.exist?(fn))
    $db = SQLite3::Database.open(fn)
  else
    $db = SQLite3::Database.new(fn)
    sql = "create table "+
      "data(id integer primary key autoincrement, " +
      "date datetime, key string, val real, unit string, "+
      "exec_time real default 0, exec_unit text default 'sec');"
    $db.execute(sql)
    sql = "create table "+
      "statistics(date datetime, key string, avr real, stddev real);"
    $db.execute(sql)
    sql = "create table "+
      "error(date datetime, count integer);"
    $db.execute(sql)
    sql = "create table "+
      "error_msg(date datetime, command text, message text);"
    $db.execute(sql)
    sql = "create table "+
      "execute_time(date datetime, end_date datetime);"
    $db.execute(sql)
    sql = "create index data_date_index on data(date);";
    $db.execute(sql)
    sql = "create index data_key_index on data(key);";
    $db.execute(sql)
    sql = "create index statistics_date_index on statistics(date);";
    $db.execute(sql)
    sql = "create index error_date_index on error(date);";
    $db.execute(sql)
    sql = "create index error_msg_date_index on error_msg(date);"
    $db.execute(sql)
    sql = "create index execute_time_date_index on execute_time(date);"
    $db.execute(sql)
  end
end

def close_db()
  $db.close
end

def backup_db()
  if ($config["database"]["backup"].nil?)
    return
  end

  begin
    FileUtils.cp($config["database"]["filename"],
                 $config["database"]["backup"])
  rescue => e
    print e.message+"\n"
  end
end

def make_opt(h)
  opt = ""
  h.each{|key,val|
    opt += " --#{key}"
    if (val)
      opt += " #{val}"
    end
  }
  return opt
end

def insert_error_message(command, message)
  $error_message_lock.synchronize {
    open_db()
    sql = "insert into error_msg(date,command,message) "+
    "values('#{$now.to_i}',?,?);"
    $db.execute(sql, command, message)
    close_db()
  }
end

def insert_execute_time()
  end_time = Time.now
  sql = "insert into execute_time(date,end_date) "+
    "values('#{$now.to_i}','#{end_time.to_i}');"
  $db.execute(sql)
end

def insert_error_result()
  if ($errcount > 0)
    sql = "insert into error(date,count) "+
      "values('#{$now.to_i}',#{$errcount});"
    $db.execute(sql)
  end
end

def insert_result(str)
  $db.execute("begin;")
  str.split("\n").each{|line|
    array=line.split(" ");
    if (array[1] == "=") 
      if (array.size == 4)
        sql="insert into data(date,key,val,unit) values(?,?,?,?);"
        $db.execute(sql,$now.to_i,array[0],array[2],array[3])
      elsif (array.size == 6)
        sql="insert into data(date,key,val,unit,exec_time,exec_unit) "+
          "values(?,?,?,?,?,?);"
        $db.execute(sql,$now.to_i,array[0],array[2],array[3],array[4],array[5])
      end
    end
  }
  $db.execute("commit;")
end

def check_server_status()
  command = "gfsched"
  tmp = ""
  errstr = ""
  Open3.popen3(command) { |stdin, stdout, stderr|
    stdin.close
    t1 = Thread.new {
      tmp = stdout.read
    }
    t2 = Thread.new {
      errstr = stderr.read
    }
    t1.join
    t2.join
  }
  if (errstr.length > 0)
    open($config['errlog'],"a") {|f|
      f.print(Time.now.to_s+"\n")
      f.print(command+"\n")
      f.print(errstr)
    }
    insert_error_message(command, errstr)
    $errcount+=1
    return false
  end
  return true
end

def do_single(key, type)
  result = ""
  if ($config[type].nil?)
    return result
  end
  $config[type].uniq.each {|param|
    command = "gfperf-wrapper.sh #{key} gfperf-#{type}" + make_opt(param)
    print command+"\n"
    tmp = ""
    errstr = ""
    Open3.popen3(command) { |stdin, stdout, stderr|
      stdin.close
      t1 = Thread.new {
        tmp = stdout.read
      }
      t2 = Thread.new {
        errstr = stderr.read
      }
      t1.join
      t2.join
    }
    if (errstr.length > 0)
      open($config['errlog'],"a") {|f|
        f.print(Time.now.to_s+"\n")
        f.print(command+"\n")
        f.print(errstr)
      }
      insert_error_message(command, errstr)
      $errcount+=1
    end
    if (tmp.length > 0)
      result += tmp.split("\n").map{|t| key+"/"+t}.join("\n")+"\n"
    end
  }
  return result
end

def do_parallel(key, name, array)
  r = Array.new
  t = Array.new
  start = Time.new+5
  wopt = " --wait #{start.utc.strftime("%Y-%m-%dT%H:%M:%SZ")}"
  array.uniq.each_with_index do |param,i|
    t[i] = Thread.new {
      host = param["rhost"]
      type = param["type"]
      param2 = param.clone
      param2.delete("rhost")
      param2.delete("type")
      command = (host) ? "ssh #{host} " : ""
      if (!$config['remote_path'].nil?)
        command += "#{$config['remote_path']}/"
      end
      command += "gfperf-wrapper.sh #{key} "
      if (!$config['remote_path'].nil?)
        command += "#{$config['remote_path']}/"
      end
      command += "gfperf-parallel-#{type} --name #{name}"
      command += wopt + make_opt(param2)
      print command+"\n"
      tmp = ""
      errstr = ""
      Open3.popen3(command) { |stdin, stdout, stderr|
        stdin.close
        t1 = Thread.new {
          tmp = stdout.read
        }
        t2 = Thread.new {
          errstr = stderr.read
        }
        t1.join
        t2.join
      }
      if (errstr.size > 0)
        open($config['errlog'],"a") {|f|
          f.print(Time.now.to_s+"\n")
          f.print(command+"\n")
          f.print(errstr)
        }
        insert_error_message(command, errstr)
        $errcount+=1
      end
      if (tmp.length > 0)
        r[i] = tmp.split("\n").map{|t| key+"/"+t}.join("\n")+"\n"
      end
    }
  end
  t.each{|thread|
    thread.join
  }
  return r.join
end

def do_parallel_autoreplica(key, name, array)
  r = Array.new
  t = Array.new
  start = Time.new+5
  wopt = " --wait #{start.utc.strftime("%Y-%m-%dT%H:%M:%SZ")}"
  array.uniq.each_with_index do |param,i|
    t[i] = Thread.new {
      command = "gfperf-wrapper.sh #{key} "
      command += "gfperf-parallel-autoreplica --name #{name}"
      command += wopt + make_opt(param)
      print command+"\n"
      tmp = ""
      errstr = ""
      Open3.popen3(command) { |stdin, stdout, stderr|
        stdin.close
        tmp = stdout.read
        errstr = stderr.read
      }
      if (errstr.size > 0)
        open($config['errlog'],"a") {|f|
          f.print(Time.now.to_s+"\n")
          f.print(command+"\n")
          f.print(errstr)
        }
        insert_error_message(command, errstr)
        $errcount+=1
      end
      if (tmp.length > 0)
        r[i] = tmp.split("\n").map{|t| key+"/"+t}.join("\n")+"\n"
      else
        r[i] = nil
      end
    }
  end
  t.each{|thread|
    thread.join
  }
  a=r.compact.map{|line| line.split(' ')[2].to_f}
  if (a.size > 0)
    avr=a.inject(0.0){|sum, el| sum += el} / a.size
    ret = r[0].split(' ')
    ret[2] = avr
    return ret.join(' ')+"\n"
  else
    return ""
  end
end

def mount_gfarm2fs(key)
  $config["gfarm2fs_mountpoint"].each {|mp|
    `gfperf-wrapper.sh #{key} gfarm2fs #{mp}`
  }
end

def umount_gfarm2fs()
  $config["gfarm2fs_mountpoint"].each {|mp|
    `fusermount -u #{mp}`
  }
end

$check_flag = false
OptionParser.new do |opt|
  opt.on('-c') {|v| $check_flag = true}
  opt.permute!(ARGV)
end

conf_file = (ARGV.size > 0) ? ARGV[0] : "config.yml"
if (!File.exist?(conf_file))
  STDERR.print("#{conf_file} not found\n")
  exit(1)
end

conf_file_fd = File.open(conf_file)
if (conf_file_fd.flock(File::LOCK_EX|File::LOCK_NB) == false)
  print "config file is locked.\n"
  print "another process is running on same config file.\n"
  exit(0)
end

Signal.trap(:INT) {
  umount_gfarm2fs()
  conf_file_fd.flock(File::LOCK_UN)
  conf_file_fd.close()
  exit(0)
}

Signal.trap(:TERM) {
  umount_gfarm2fs()
  conf_file_fd.flock(File::LOCK_UN)
  conf_file_fd.close()
  exit(0)
}

$now = Time.now
$config = YAML.load(File.read(conf_file))
if ($check_flag == true)
  conf_file_fd.flock(File::LOCK_UN)
  conf_file_fd.close()
  print "#{conf_file} is ok.\n"
  exit(0)
end

$errcount = 0
result = ""
r = check_server_status()
if (!r)
  open_db()
  insert_error_result()
  insert_execute_time()
  close_db()
  backup_db()

  conf_file_fd.flock(File::LOCK_UN)
  conf_file_fd.close()
  exit(1)
end

$config["authentication"].each do |key|
  mount_gfarm2fs(key)

  types = ["metadata", "tree", "copy", "read", "write",
           "replica", "autoreplica"]
  types.each {|type|
    result += do_single(key, type)
  }
  if (!$config["parallel"].nil?)
    $config["parallel"].each {|name, array|
      result += do_parallel(key, name, array)
    }
  end
  if (!$config["parallel-autoreplica"].nil?)
    $config["parallel-autoreplica"].each {|name, array|
      result += do_parallel_autoreplica(key, name, array)
    }
  end

  umount_gfarm2fs()
end

open_db()

insert_result(result)
insert_error_result()
calc_stddev()
insert_execute_time()

close_db()

backup_db()

conf_file_fd.flock(File::LOCK_UN)
conf_file_fd.close()
