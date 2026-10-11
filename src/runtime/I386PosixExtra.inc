// Additional i386 POSIX imports used by Portal and its bundled libraries.
#include <pwd.h>
#include <sys/resource.h>
#include <sys/mman.h>
#ifndef __EMSCRIPTEN__
#include <sys/mount.h>
#endif
#include <sys/uio.h>
#include <termios.h>
#ifndef __EMSCRIPTEN__
#include <sys/file.h>
#endif
#include <link.h>
#include <elf.h>

namespace {
int store_stat32_extra(uint8_t *arena, uint32_t address, const struct stat &host) {
  if (host.st_size < INT32_MIN || host.st_size > INT32_MAX ||
      uint64_t(host.st_ino) > UINT32_MAX || host.st_atim.tv_sec < INT32_MIN ||
      host.st_atim.tv_sec > INT32_MAX || host.st_mtim.tv_sec < INT32_MIN ||
      host.st_mtim.tv_sec > INT32_MAX || host.st_ctim.tv_sec < INT32_MIN ||
      host.st_ctim.tv_sec > INT32_MAX) { errno = EOVERFLOW; return -1; }
  uint8_t data[88] = {};
  auto put = [&](unsigned o, uint64_t v, unsigned n = 4) { memcpy(data + o, &v, n); };
  put(0, host.st_dev, 8); put(12, host.st_ino); put(16, host.st_mode);
  put(20, host.st_nlink); put(24, host.st_uid); put(28, host.st_gid);
  put(32, host.st_rdev, 8); put(44, host.st_size); put(48, host.st_blksize);
  put(52, host.st_blocks); put(56, host.st_atim.tv_sec); put(60, host.st_atim.tv_nsec);
  put(64, host.st_mtim.tv_sec); put(68, host.st_mtim.tv_nsec);
  put(72, host.st_ctim.tv_sec); put(76, host.st_ctim.tv_nsec);
  memcpy(guest(arena, address, sizeof(data)), data, sizeof(data));
  return 0;
}
uint32_t extra_next_file = 0x70000000;
std::map<uint32_t, FILE *> extra_pipes;
struct ExtraMapping { uint32_t length; int fd; uint64_t offset; int flags; uint32_t prot; uint32_t file_bytes; };
std::map<uint32_t, ExtraMapping> extra_mappings;
std::map<uint32_t, uint32_t> extra_free_mappings{{uint32_t(MMAP_START_VMA),uint32_t(MMAP_REGION_SIZE)}};
uint32_t extra_passwd_record=0;
uint32_t extra_passwd_strings[5]={};
std::mutex extra_mapping_mutex;
std::mutex extra_passwd_mutex;
uint32_t extra_string(uint8_t *arena,const char *value,unsigned slot){return copy_string(arena,value,slot);}
int extra_flush_mapping(uint8_t *arena,uint32_t address,uint32_t length,int fd,uint64_t offset) {
  uint32_t done=0;
  while(done<length) {
    ssize_t n=pwrite(fd,guest(arena,address+done,length-done),length-done,off_t(offset+done));
    if(n<0){if(errno==EINTR)continue;return -1;}
    if(!n){errno=EIO;return -1;}
    done+=uint32_t(n);
  }
  return 0;
}
void extra_iovec(uint8_t *arena, uint32_t address, uint32_t count,
                 std::vector<struct iovec> &host) {
  if (count > 1024) { errno = EINVAL; return; }
  const uint8_t *data = count ? static_cast<const uint8_t *>(guest(arena, address, uint64_t(count) * 8)) : nullptr;
  host.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t base, len;
    memcpy(&base, data + i * 8, 4); memcpy(&len, data + i * 8 + 4, 4);
    host.push_back({len ? guest(arena, base, len) : nullptr, len});
  }
}
struct ExtraScandirEntry { uint32_t address; };
}

IMPORT(__fxstat) {
  CALL; struct stat st;
  int r = fstat(int32_t(call.word(1)), &st);
  if (!r) r = store_stat32_extra(arena, call.word(2), st);
  call.result(r);
}
IMPORT(__lxstat) {
  CALL; struct stat st; int r = lstat(text(arena, call.word(1)), &st);
  if (!r) r = store_stat32_extra(arena, call.word(2), st);
  call.result(r);
}
IMPORT(__xstat) {
  CALL; struct stat st; int r = stat(text(arena, call.word(1)), &st);
  if (!r) r = store_stat32_extra(arena, call.word(2), st);
  call.result(r);
}
IMPORT(__xmknod) {
  CALL;dev_t dev=0;
  if(call.word(3))memcpy(&dev,guest(arena,call.word(3),sizeof(dev)),sizeof(dev));
  call.result(mknod(text(arena,call.word(1)),call.word(2),dev));
}
IMPORT(__xpg_basename) {
  CALL; uint32_t a=call.word(0);char *s=static_cast<char *>(guest(arena,a,1));
  if(!*s){call.result(copy_string(arena,".",18));return;}
  char *end=s+strlen(s);while(end>s+1&&end[-1]=='/')--end;
  char *begin=end;while(begin>s&&begin[-1]!='/')--begin;
  if(begin==end){s[0]='/';s[1]=0;call.result(a);return;}
  *end=0;call.result(a+uint32_t(begin-s));
}
IMPORT(alphasort64) {
  CALL; uint32_t a=call.word(0),b=call.word(1),first,second;
  memcpy(&first,guest(arena,a,4),4);memcpy(&second,guest(arena,b,4),4);
  call.result(strcoll(static_cast<const char *>(guest(arena,first+19,1)),
                      static_cast<const char *>(guest(arena,second+19,1))));
}
IMPORT(ctermid) {
  CALL;uint32_t address=call.word(0);
  if(address){const char *s="/dev/tty";memcpy(guest(arena,address,strlen(s)+1),s,strlen(s)+1);call.result(address);}
  else call.result(extra_string(arena,"/dev/tty",10));
}
IMPORT(dup) { CALL; call.result(dup(int32_t(call.word(0)))); }
IMPORT(fcntl) {
  CALL;const int fd=int32_t(call.word(0)),cmd=int32_t(call.word(1));uint32_t arg=call.word(2);
  int r=-1;
  if(cmd==F_GETLK||cmd==F_SETLK||cmd==F_SETLKW) {
    auto *p=static_cast<uint8_t *>(guest(arena,arg,16));struct flock f{};
    int16_t type,whence;int32_t start,len,pid;
    memcpy(&type,p,2);memcpy(&whence,p+2,2);memcpy(&start,p+4,4);memcpy(&len,p+8,4);memcpy(&pid,p+12,4);
    f.l_type=type;f.l_whence=whence;f.l_start=start;f.l_len=len;f.l_pid=pid;
    r=fcntl(fd,cmd,&f);
    if(cmd==F_GETLK&&r!=-1) {
      if(f.l_start<INT32_MIN||f.l_start>INT32_MAX||f.l_len<INT32_MIN||f.l_len>INT32_MAX){errno=EOVERFLOW;r=-1;}
      else {type=f.l_type;whence=f.l_whence;start=int32_t(f.l_start);len=int32_t(f.l_len);pid=f.l_pid;
        memcpy(p,&type,2);memcpy(p+2,&whence,2);memcpy(p+4,&start,4);memcpy(p+8,&len,4);memcpy(p+12,&pid,4);}
    }
  }
  else if(cmd==12||cmd==13||cmd==14) {
    auto *p=static_cast<uint8_t *>(guest(arena,arg,24));struct flock f{};
    int16_t type,whence;int64_t start,len;int32_t pid;
    memcpy(&type,p,2);memcpy(&whence,p+2,2);memcpy(&start,p+4,8);memcpy(&len,p+12,8);memcpy(&pid,p+20,4);
    f.l_type=type;f.l_whence=whence;f.l_start=start;f.l_len=len;f.l_pid=pid;
    const int hostcmd=cmd==12?F_GETLK:cmd==13?F_SETLK:F_SETLKW;
    r=fcntl(fd,hostcmd,&f);
    if(cmd==12&&r!=-1){type=f.l_type;whence=f.l_whence;start=f.l_start;len=f.l_len;pid=f.l_pid;
      memcpy(p,&type,2);memcpy(p+2,&whence,2);memcpy(p+4,&start,8);memcpy(p+12,&len,8);memcpy(p+20,&pid,4);}
  }
#ifdef F_GETOWN_EX
  else if(cmd==F_GETOWN_EX||cmd==F_SETOWN_EX) {
    auto *p=static_cast<uint8_t *>(guest(arena,arg,8));struct f_owner_ex owner{};
    if(cmd==F_SETOWN_EX){memcpy(&owner.type,p,4);memcpy(&owner.pid,p+4,4);}
    r=fcntl(fd,cmd,&owner);
    if(cmd==F_GETOWN_EX){memcpy(p,&owner.type,4);memcpy(p+4,&owner.pid,4);}
  }
#endif
  else {
    switch(cmd) {
      case F_GETFD:case F_GETFL:case F_GETOWN:r=fcntl(fd,cmd);break;
      case F_DUPFD:case F_SETFD:case F_SETFL:case F_SETOWN:
#ifdef F_DUPFD_CLOEXEC
      case F_DUPFD_CLOEXEC:
#endif
#ifdef F_SETSIG
      case F_SETSIG:
#endif
#ifdef F_GETSIG
      case F_GETSIG:
#endif
#ifdef F_SETLEASE
      case F_SETLEASE:
#endif
#ifdef F_GETLEASE
      case F_GETLEASE:
#endif
#ifdef F_NOTIFY
      case F_NOTIFY:
#endif
#ifdef F_SETPIPE_SZ
      case F_SETPIPE_SZ:
#endif
#ifdef F_GETPIPE_SZ
      case F_GETPIPE_SZ:
#endif
        r=fcntl(fd,cmd,int32_t(arg));break;
      default:errno=EINVAL;r=-1;break;
    }
  }
  call.result(r);
}
IMPORT(fileno_unlocked) { CALL; call.result(fileno(stream(call.word(0)))); }
IMPORT(flock) {
  CALL;
#ifdef __EMSCRIPTEN__
  errno=ENOSYS;call.result(-1);
#else
  call.result(flock(int32_t(call.word(0)),int32_t(call.word(1))));
#endif
}
IMPORT(getegid) { CALL; call.result(getegid()); }
IMPORT(geteuid) { CALL; call.result(geteuid()); }
IMPORT(getgid) { CALL; call.result(getgid()); }
IMPORT(getuid) { CALL; call.result(getuid()); }
IMPORT(getpwuid) {
  CALL;std::lock_guard<std::mutex> lock(extra_passwd_mutex);struct passwd *p=getpwuid(call.word(0));
  if(!p){call.result(0);return;}
  uint32_t fresh[5]={};const char *src[5]={p->pw_name,p->pw_passwd,p->pw_gecos,p->pw_dir,p->pw_shell};
  for(unsigned i=0;i<5;++i)if(src[i]&&!(fresh[i]=owned_string(arena,src[i]))) {
    for(unsigned j=0;j<i;++j)if(fresh[j])release(fresh[j]);
    call.result(0);return;
  }
  if(!extra_passwd_record)extra_passwd_record=allocate(28);
  if(!extra_passwd_record){for(uint32_t a:fresh)if(a)release(a);call.result(0);return;}
  uint32_t data[7]={fresh[0],fresh[1],uint32_t(p->pw_uid),uint32_t(p->pw_gid),fresh[2],fresh[3],fresh[4]};
  memcpy(guest(arena,extra_passwd_record,sizeof(data)),data,sizeof(data));
  for(uint32_t a:extra_passwd_strings)if(a)release(a);
  memcpy(extra_passwd_strings,fresh,sizeof(fresh));call.result(extra_passwd_record);
}
IMPORT(getrusage) {
  CALL; struct rusage u; int r = getrusage(int32_t(call.word(0)), &u);
  if (!r) {
    int64_t raw[18]={int64_t(u.ru_utime.tv_sec),int64_t(u.ru_utime.tv_usec),
      int64_t(u.ru_stime.tv_sec),int64_t(u.ru_stime.tv_usec),int64_t(u.ru_maxrss),
      int64_t(u.ru_ixrss),int64_t(u.ru_idrss),int64_t(u.ru_isrss),int64_t(u.ru_minflt),
      int64_t(u.ru_majflt),int64_t(u.ru_nswap),int64_t(u.ru_inblock),int64_t(u.ru_oublock),
      int64_t(u.ru_msgsnd),int64_t(u.ru_msgrcv),int64_t(u.ru_nsignals),
      int64_t(u.ru_nvcsw),int64_t(u.ru_nivcsw)};
    int32_t vals[18];
    for(unsigned i=0;i<18;++i) {
      if(raw[i]<INT32_MIN||raw[i]>INT32_MAX){errno=EOVERFLOW;r=-1;break;}
      vals[i]=int32_t(raw[i]);
    }
    if(!r){uint8_t d[72]={};memcpy(d,vals,sizeof(vals));memcpy(guest(arena,call.word(1),sizeof(d)),d,sizeof(d));}
  }
  call.result(r);
}
IMPORT(getrlimit) {
  CALL;
  const uint32_t address = call.word(1);
  if (!address) { errno = EFAULT; call.result(-1); return; }
  struct rlimit limits;
  const int result = ::getrlimit(int32_t(call.word(0)), &limits);
  if (result == 0) {
    const auto guest_limit = [](rlim_t value) {
      return value == RLIM_INFINITY || value > UINT32_MAX
          ? UINT32_MAX : uint32_t(value);
    };
    uint32_t values[2] = {guest_limit(limits.rlim_cur), guest_limit(limits.rlim_max)};
    memcpy(guest(arena, address, sizeof(values)), values, sizeof(values));
  }
  call.result(result);
}
IMPORT(setrlimit) {
  CALL;
  const uint32_t address = call.word(1);
  if (!address) { errno = EFAULT; call.result(-1); return; }
  uint32_t values[2];
  memcpy(values, guest(arena, address, sizeof(values)), sizeof(values));
  struct rlimit limits = {
    values[0] == UINT32_MAX ? RLIM_INFINITY : rlim_t(values[0]),
    values[1] == UINT32_MAX ? RLIM_INFINITY : rlim_t(values[1])
  };
  call.result(::setrlimit(int32_t(call.word(0)), &limits));
}
IMPORT(isatty) { CALL; call.result(isatty(int32_t(call.word(0)))); }
IMPORT(mallinfo) {
  CALL;call.return_argument_bytes=4;uint32_t address=call.word(0);
  int32_t d[10]={};std::lock_guard<std::recursive_mutex> lock(allocation_mutex);
  uint64_t total=0,used=0,free=0;int32_t free_blocks=0;
  for(const auto &entry:allocations){total+=entry.second.size;if(entry.second.free){free+=entry.second.size;++free_blocks;}else used+=entry.second.size;}
  d[0]=int32_t(total);d[1]=free_blocks;d[7]=int32_t(used);d[8]=int32_t(free);
  if(!allocations.empty()&&allocations.rbegin()->second.free)d[9]=int32_t(allocations.rbegin()->second.size);
  memcpy(guest(arena,address,sizeof(d)),d,sizeof(d));call.result(address);
}
IMPORT(malloc_usable_size) {
  CALL; uint32_t a=call.word(0); std::lock_guard<std::recursive_mutex> lock(allocation_mutex);
  auto it=allocations.find(a); call.result(it != allocations.end() && !it->second.free ? it->second.size : 0);
}
IMPORT(memalign) {
  CALL; uint32_t align=call.word(0), size=call.word(1);
  if (!align || (align&(align-1))) { errno=EINVAL; call.result(0); return; }
  call.result(allocate_aligned(size,std::max(align,16u)));
}
IMPORT(mkfifo) { CALL; call.result(mkfifo(text(arena,call.word(0)),call.word(1))); }
namespace {
void extra_mmap(Call &call, bool wide_offset) {
  uint8_t *arena = call.arena;
  const uint32_t requested=call.word(0),length=call.word(1),prot=call.word(2);
  const int flags=int32_t(call.word(3)),fd=int32_t(call.word(4));
  const uint64_t offset=wide_offset?call.wide(5):call.word(5);
  std::lock_guard<std::mutex> mapping_lock(extra_mapping_mutex);
  constexpr uint32_t page=4096,map_fixed_noreplace=0x100000;
  constexpr uint32_t supported_map_flags=MAP_SHARED|MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS|map_fixed_noreplace;
  const uint32_t type=uint32_t(flags)&(MAP_SHARED|MAP_PRIVATE);
  if(!length||(offset&(page-1))||offset>uint64_t(INT64_MAX)||
     (prot&~uint32_t(PROT_READ|PROT_WRITE|PROT_EXEC))||
     (type!=MAP_SHARED&&type!=MAP_PRIVATE&&type!=3)||
     (uint32_t(flags)&~supported_map_flags)||
     ((flags&MAP_ANONYMOUS)&&offset)||((flags&MAP_FIXED)&&(uint32_t(flags)&map_fixed_noreplace))||
     (!(flags&MAP_ANONYMOUS)&&offset+uint64_t(length)>uint64_t(INT64_MAX))) {
    errno=EINVAL;call.result(UINT32_MAX);return;
  }
  const uint64_t rounded64=(uint64_t(length)+page-1)&~uint64_t(page-1);
  if(rounded64>MMAP_REGION_SIZE){errno=ENOMEM;call.result(UINT32_MAX);return;}
  const uint32_t rounded=uint32_t(rounded64);
  const bool noreplace=(uint32_t(flags)&map_fixed_noreplace)!=0;
  const bool fixed=(flags&MAP_FIXED)!=0||noreplace;
  int owned_fd=(flags&MAP_ANONYMOUS)?-1:dup(fd);uint32_t file_bytes=0;
  if(!(flags&MAP_ANONYMOUS)) {
    if(owned_fd<0){call.result(UINT32_MAX);return;}
    struct stat backing{};
    if(fstat(owned_fd,&backing)<0){int e=errno;close(owned_fd);errno=e;call.result(UINT32_MAX);return;}
    if(backing.st_size>0&&uint64_t(backing.st_size)>offset)
      file_bytes=uint32_t(std::min<uint64_t>(length,uint64_t(backing.st_size)-offset));
  }
  auto add_free=[&](uint32_t b,uint32_t n) {
    if(!n)return;
    auto p=extra_free_mappings.lower_bound(b);
    if(p!=extra_free_mappings.begin()){auto q=std::prev(p);if(uint64_t(q->first)+q->second==b){b=q->first;n+=q->second;extra_free_mappings.erase(q);}}
    p=extra_free_mappings.lower_bound(b);
    if(p!=extra_free_mappings.end()&&uint64_t(b)+n==p->first){n+=p->second;extra_free_mappings.erase(p);}
    extra_free_mappings.emplace(b,n);
  };
  uint32_t address=0;
  if(fixed) {
    address=requested;const uint64_t end=uint64_t(address)+rounded;
    if(address%page||address<MMAP_START_VMA||end>MMAP_END_VMA){if(owned_fd>=0)close(owned_fd);errno=EINVAL;call.result(UINT32_MAX);return;}
    auto f=extra_free_mappings.upper_bound(address);
    if(f!=extra_free_mappings.begin())--f;
    const bool entirely_free=f!=extra_free_mappings.end()&&address>=f->first&&end<=uint64_t(f->first)+f->second;
    if(noreplace&&!entirely_free){if(owned_fd>=0)close(owned_fd);errno=EEXIST;call.result(UINT32_MAX);return;}
    if(noreplace) {
      uint32_t b=f->first,n=f->second;extra_free_mappings.erase(f);
      if(address>b)extra_free_mappings.emplace(b,address-b);
      if(end<uint64_t(b)+n)extra_free_mappings.emplace(uint32_t(end),uint32_t(uint64_t(b)+n-end));
    } else {
      auto it=extra_mappings.lower_bound(address);if(it!=extra_mappings.begin())--it;
      while(it!=extra_mappings.end()) {
        uint32_t b=it->first;ExtraMapping m=it->second;uint64_t e=uint64_t(b)+m.length;
        if(e<=address){++it;continue;}if(b>=end)break;
        const uint32_t prefix=b<address?address-b:0,tail=uint32_t(e>end?e-end:0);
        int prefix_fd=prefix&&m.fd>=0?dup(m.fd):-1,tail_fd=tail&&m.fd>=0?dup(m.fd):-1;
        if((prefix&&m.fd>=0&&prefix_fd<0)||(tail&&m.fd>=0&&tail_fd<0)){
          if(prefix_fd>=0)close(prefix_fd);if(tail_fd>=0)close(tail_fd);if(owned_fd>=0)close(owned_fd);
          call.result(UINT32_MAX);return;
        }
        if((m.flags&MAP_SHARED)&&(m.prot&PROT_WRITE)&&m.fd>=0&&m.file_bytes&&
           extra_flush_mapping(arena,b,m.file_bytes,m.fd,m.offset)<0) {
          if(prefix_fd>=0)close(prefix_fd);if(tail_fd>=0)close(tail_fd);if(owned_fd>=0)close(owned_fd);
          call.result(UINT32_MAX);return;
        }
        it=extra_mappings.erase(it);
        if(prefix)extra_mappings.emplace(b,ExtraMapping{prefix,prefix_fd,m.offset,m.flags,m.prot,std::min(prefix,m.file_bytes)});
        if(tail)extra_mappings.emplace(uint32_t(end),ExtraMapping{tail,tail_fd,m.offset+end-b,m.flags,m.prot,
            m.file_bytes>end-b?std::min(tail,uint32_t(m.file_bytes-(end-b))):0});
        if(m.fd>=0)close(m.fd);
      }
      auto p=extra_free_mappings.lower_bound(address);if(p!=extra_free_mappings.begin())--p;
      while(p!=extra_free_mappings.end()) {
        uint32_t b=p->first,n=p->second;uint64_t e=uint64_t(b)+n;
        if(e<=address){++p;continue;}if(b>=end)break;p=extra_free_mappings.erase(p);
        if(b<address)extra_free_mappings.emplace(b,address-b);
        if(e>end)extra_free_mappings.emplace(uint32_t(end),uint32_t(e-end));
      }
    }
  } else {
    const uint32_t hint_address=requested&~(page-1);
    auto hint=extra_free_mappings.upper_bound(hint_address);
    if(requested&&hint_address>=MMAP_START_VMA&&hint!=extra_free_mappings.begin()) {
      --hint;
      if(hint_address>=hint->first&&uint64_t(hint_address)+rounded<=uint64_t(hint->first)+hint->second) {
        uint32_t b=hint->first,n=hint->second;extra_free_mappings.erase(hint);
        if(hint_address>b)extra_free_mappings.emplace(b,hint_address-b);
        uint32_t after=hint_address+rounded;
        if(uint64_t(after)<uint64_t(b)+n)extra_free_mappings.emplace(after,uint32_t(uint64_t(b)+n-after));
        address=hint_address;
      }
    }
    if(!address) {
      auto p=extra_free_mappings.begin();for(;p!=extra_free_mappings.end()&&p->second<rounded;++p){}
      if(p==extra_free_mappings.end()){if(owned_fd>=0)close(owned_fd);errno=ENOMEM;call.result(UINT32_MAX);return;}
      address=p->first;uint32_t left=p->second-rounded;extra_free_mappings.erase(p);
      if(left)extra_free_mappings.emplace(address+rounded,left);
    }
  }
  memset(guest(arena,address,rounded),0,rounded);
  if(owned_fd>=0&&file_bytes) {
    ssize_t n=pread(owned_fd,guest(arena,address,file_bytes),file_bytes,off_t(offset));
    if(n<0){int e=errno;close(owned_fd);add_free(address,rounded);memset(guest(arena,address,rounded),0,rounded);errno=e;call.result(UINT32_MAX);return;}
    file_bytes=std::min(file_bytes,uint32_t(n));
  }
  extra_mappings[address]={rounded,owned_fd,offset,flags,prot,file_bytes};
  call.result(address);
}
}
IMPORT(mmap) { CALL; extra_mmap(call, false); }
IMPORT(mmap64) { CALL; extra_mmap(call, true); }
IMPORT(mount) {
  CALL;
#ifdef __EMSCRIPTEN__
  errno=ENOSYS; call.result(-1);
#else
  call.result(mount(call.word(0)?text(arena,call.word(0)):nullptr,
      call.word(1)?text(arena,call.word(1)):nullptr,
      call.word(2)?text(arena,call.word(2)):nullptr,call.word(3),
      call.word(4)?guest(arena,call.word(4),1):nullptr));
#endif
}
IMPORT(munmap) {
  std::lock_guard<std::mutex> mapping_lock(extra_mapping_mutex);
  CALL;uint32_t start=call.word(0),length=call.word(1);constexpr uint32_t page=4096;
  if(!length||start%page){errno=EINVAL;call.result(-1);return;}
  uint64_t rounded=(uint64_t(length)+page-1)&~uint64_t(page-1);
  if(rounded>UINT32_MAX||start<MMAP_START_VMA||uint64_t(start)+rounded>MMAP_END_VMA){errno=EINVAL;call.result(-1);return;}
  length=uint32_t(rounded);
  uint64_t end=uint64_t(start)+length,cursor=start;auto it=extra_mappings.lower_bound(start);
  if(it!=extra_mappings.begin())--it;
  for(auto p=it;p!=extra_mappings.end()&&cursor<end;++p) {
    uint64_t b=p->first,e=b+p->second.length;if(e<=cursor)continue;
    if(b>cursor)break;cursor=std::min<uint64_t>(e,end);
  }
  if(cursor!=end){errno=EINVAL;call.result(-1);return;}
  auto add_free=[&](uint32_t b,uint32_t n) {
    auto p=extra_free_mappings.lower_bound(b);
    if(p!=extra_free_mappings.begin()){auto q=std::prev(p);if(uint64_t(q->first)+q->second==b){b=q->first;n+=q->second;extra_free_mappings.erase(q);}}
    p=extra_free_mappings.lower_bound(b);if(p!=extra_free_mappings.end()&&uint64_t(b)+n==p->first){n+=p->second;extra_free_mappings.erase(p);}
    extra_free_mappings.emplace(b,n);
  };
  it=extra_mappings.lower_bound(start);if(it!=extra_mappings.begin())--it;
  for(auto p=it;p!=extra_mappings.end();++p) {
    uint32_t b=p->first;const ExtraMapping &m=p->second;uint64_t e=uint64_t(b)+m.length;
    if(e<=start)continue;if(b>=end)break;
    uint32_t lo=std::max(b,start),hi=uint32_t(std::min(e,end));
    uint32_t backed=lo-b<m.file_bytes?std::min(hi-lo,uint32_t(m.file_bytes-(lo-b))):0;
    if((m.flags&MAP_SHARED)&&(m.prot&PROT_WRITE)&&m.fd>=0&&backed&&extra_flush_mapping(arena,lo,backed,m.fd,m.offset+lo-b)<0){call.result(-1);return;}
  }
  it=extra_mappings.lower_bound(start);if(it!=extra_mappings.begin())--it;
  while(it!=extra_mappings.end()) {
    uint32_t b=it->first;ExtraMapping m=it->second;uint64_t e=uint64_t(b)+m.length;
    if(e<=start){++it;continue;}if(b>=end)break;
    uint32_t lo=std::max(b,start),hi=uint32_t(std::min(e,end));
    uint32_t prefix=lo-b,tail=uint32_t(e-hi),skip=hi-b;
    int prefix_fd=prefix&&m.fd>=0?dup(m.fd):-1,tail_fd=tail&&m.fd>=0?dup(m.fd):-1;
    if((prefix&&m.fd>=0&&prefix_fd<0)||(tail&&m.fd>=0&&tail_fd<0)){
      if(prefix_fd>=0)close(prefix_fd);if(tail_fd>=0)close(tail_fd);call.result(-1);return;
    }
    it=extra_mappings.erase(it);
    if(prefix)extra_mappings.emplace(b,ExtraMapping{prefix,prefix_fd,m.offset,m.flags,m.prot,std::min(prefix,m.file_bytes)});
    if(tail)extra_mappings.emplace(hi,ExtraMapping{tail,tail_fd,m.offset+skip,m.flags,m.prot,m.file_bytes>skip?std::min(tail,uint32_t(m.file_bytes-skip)):0});
    if(m.fd>=0)close(m.fd);
  }
  add_free(start,length);memset(guest(arena,start,length),0,length);call.result(0);
}
IMPORT(popen) {
  CALL;
#ifdef __EMSCRIPTEN__
  errno=ENOSYS; call.result(0);
#else
  FILE *f=popen(text(arena,call.word(0)),text(arena,call.word(1)));
  if (!f) { call.result(0); return; }
  uint32_t h=extra_next_file++;
  if (h < 0x70000000) { pclose(f); errno=EMFILE; call.result(0); return; }
  extra_pipes.emplace(h,f); files.emplace(h,f); call.result(h);
#endif
}
IMPORT(pclose) {
  CALL; uint32_t h=call.word(0); auto it=extra_pipes.find(h);
  if (it==extra_pipes.end()) { errno=EBADF; call.result(-1); return; }
#ifdef __EMSCRIPTEN__
  errno=ENOSYS; call.result(-1);
#else
  int r=pclose(it->second); extra_pipes.erase(it); files.erase(h); call.result(r);
#endif
}
IMPORT(scandir) {
  CALL;const char *path=text(arena,call.word(0));uint32_t out=call.word(1),filter=call.word(2),cmp=call.word(3);
  DIR *dir=opendir(path);if(!dir){call.result(-1);return;}
  std::vector<ExtraScandirEntry> entries;errno=0;
  auto discard=[&](){for(auto &e:entries)release(e.address);entries.clear();};
  dirent *e=nullptr;
  while((errno=0,(e=readdir(dir))!=nullptr)) {
    if(uint64_t(e->d_ino)>UINT32_MAX||e->d_off<INT32_MIN||e->d_off>INT32_MAX){
      closedir(dir);discard();errno=EOVERFLOW;call.result(-1);return;
    }
    uint32_t a=allocate(268);if(!a){closedir(dir);discard();call.result(-1);return;}
    uint8_t d[268]={};uint32_t ino=uint32_t(e->d_ino),off=uint32_t(e->d_off);
    uint16_t reclen=uint16_t(11+strlen(e->d_name)+1);
    memcpy(d,&ino,4);memcpy(d+4,&off,4);memcpy(d+8,&reclen,2);d[10]=e->d_type;
    memcpy(d+11,e->d_name,strlen(e->d_name)+1);memcpy(guest(arena,a,sizeof(d)),d,sizeof(d));
    if(filter){uint32_t args[]={a};invoke(arena,state,runtime,filter,args,1);if(!state->gpr.rax.dword){release(a);continue;}}
    entries.push_back({a});
  }
  int err=errno;closedir(dir);if(err){discard();errno=err;call.result(-1);return;}
  uint32_t sort_args=cmp?allocate(8):0;
  if(cmp&&!sort_args){discard();call.result(-1);return;}
  if(cmp)std::sort(entries.begin(),entries.end(),[&](const auto &a,const auto &b){
    uint32_t pair[2]={a.address,b.address};memcpy(guest(arena,sort_args,8),pair,8);
    uint32_t args[]={sort_args,sort_args+4};invoke(arena,state,runtime,cmp,args,2);
    return int32_t(state->gpr.rax.dword)<0;
  });
  if(sort_args)release(sort_args);
  uint32_t list=entries.empty()?0:allocate(uint32_t(entries.size()*4));
  if(entries.size()&&!list){discard();call.result(-1);return;}
  for(size_t i=0;i<entries.size();++i)memcpy(guest(arena,list+uint32_t(i*4),4),&entries[i].address,4);
  int n=int(entries.size());memcpy(guest(arena,out,4),&list,4);call.result(n);
}
IMPORT(scandir64) {
  CALL;const char *path=text(arena,call.word(0));uint32_t out=call.word(1),filter=call.word(2),cmp=call.word(3);
  DIR *dir=opendir(path);if(!dir){call.result(-1);return;}
  std::vector<ExtraScandirEntry> entries;errno=0;
  auto discard=[&](){for(auto &e:entries)release(e.address);entries.clear();};
  dirent *e=nullptr;
  while((errno=0,(e=readdir(dir))!=nullptr)) {
    uint32_t a=allocate(276);if(!a){closedir(dir);discard();call.result(-1);return;}
    uint8_t d[276]={};uint64_t ino=e->d_ino;int64_t off=e->d_off;
    uint16_t reclen=uint16_t((19+strlen(e->d_name)+1+7)&~size_t(7));
    memcpy(d,&ino,8);memcpy(d+8,&off,8);memcpy(d+16,&reclen,2);d[18]=e->d_type;
    memcpy(d+19,e->d_name,strlen(e->d_name)+1);memcpy(guest(arena,a,sizeof(d)),d,sizeof(d));
    if(filter){uint32_t args[]={a};invoke(arena,state,runtime,filter,args,1);if(!state->gpr.rax.dword){release(a);continue;}}
    entries.push_back({a});
  }
  int err=errno;closedir(dir);if(err){discard();errno=err;call.result(-1);return;}
  uint32_t sort_args=cmp?allocate(8):0;
  if(cmp&&!sort_args){discard();call.result(-1);return;}
  if(cmp)std::sort(entries.begin(),entries.end(),[&](const auto &a,const auto &b){
    uint32_t pair[2]={a.address,b.address};memcpy(guest(arena,sort_args,8),pair,8);
    uint32_t args[]={sort_args,sort_args+4};invoke(arena,state,runtime,cmp,args,2);
    return int32_t(state->gpr.rax.dword)<0;
  });
  if(sort_args)release(sort_args);
  uint32_t list=entries.empty()?0:allocate(uint32_t(entries.size()*4));
  if(entries.size()&&!list){discard();call.result(-1);return;}
  for(size_t i=0;i<entries.size();++i)memcpy(guest(arena,list+uint32_t(i*4),4),&entries[i].address,4);
  int n=int(entries.size());memcpy(guest(arena,out,4),&list,4);call.result(n);
}
IMPORT(setbuf) {
  CALL; FILE *f=stream(call.word(0)); uint32_t b=call.word(1);
  setbuf(f,b?static_cast<char *>(guest(arena,b,BUFSIZ)):nullptr);
}
IMPORT(shm_open) {
  CALL;
#ifdef __EMSCRIPTEN__
  errno=ENOSYS; call.result(-1);
#else
  call.result(shm_open(text(arena,call.word(0)),int32_t(call.word(1)),call.word(2)));
#endif
}
IMPORT(system) {
  CALL;
#ifdef __EMSCRIPTEN__
  if(!call.word(0)){call.result(0);return;}
  errno=ENOSYS; call.result(-1);
#else
  call.result(system(call.word(0)?text(arena,call.word(0)):nullptr));
#endif
}
IMPORT(tcgetattr) {
  CALL; struct termios t; int r=tcgetattr(int32_t(call.word(0)),&t);
  if(!r) {
    uint8_t d[60]={}; uint32_t v[4]={t.c_iflag,t.c_oflag,t.c_cflag,t.c_lflag};
    memcpy(d,v,16); memcpy(d+17,t.c_cc,std::min(sizeof(t.c_cc),size_t(32)));
    uint32_t is=cfgetispeed(&t),os=cfgetospeed(&t);
    memcpy(d+52,&is,4);memcpy(d+56,&os,4);memcpy(guest(arena,call.word(1),60),d,60);
  }
  call.result(r);
}
IMPORT(tcsetattr) {
  CALL; uint8_t d[60];memcpy(d,guest(arena,call.word(2),60),60);struct termios t{};
  memcpy(&t.c_iflag,d,4);memcpy(&t.c_oflag,d+4,4);memcpy(&t.c_cflag,d+8,4);
  memcpy(&t.c_lflag,d+12,4);memcpy(t.c_cc,d+17,std::min(sizeof(t.c_cc),size_t(32)));
  uint32_t is,os;memcpy(&is,d+52,4);memcpy(&os,d+56,4);
  if(cfsetispeed(&t,is)<0||cfsetospeed(&t,os)<0){call.result(-1);return;}
  call.result(tcsetattr(int32_t(call.word(0)),int32_t(call.word(1)),&t));
}
IMPORT(tmpnam) {
  CALL;char name[L_tmpnam];
  if(!::tmpnam(name)){call.result(0);return;}
  uint32_t address=call.word(0);
  if(address){memcpy(guest(arena,address,L_tmpnam),name,strlen(name)+1);call.result(address);}
  else call.result(owned_string(arena,name));
}
IMPORT(mkstemp) {
  CALL;
  const uint32_t address = call.word(0);
  std::string pattern(text(arena, address));
  const int fd = ::mkstemp(pattern.data());
  if (fd >= 0) memcpy(guest(arena, address, pattern.size() + 1), pattern.c_str(), pattern.size() + 1);
  call.result(fd);
}
IMPORT(writev) {
  CALL; std::vector<struct iovec> v;
  if (call.word(2)>1024) { errno=EINVAL; call.result(-1); return; }
  extra_iovec(arena,call.word(1),call.word(2),v);
  ssize_t r=writev(int32_t(call.word(0)),v.data(),int(v.size()));call.result(r);
}
IMPORT(dl_iterate_phdr) {
  CALL;uint32_t callback=call.word(0),opaque=call.word(1);
  if(!callback){errno=EINVAL;call.result(-1);return;}
  if(_ecv_e_phent!=sizeof(Elf32_Phdr)||_ecv_e_phnum>UINT16_MAX){errno=ENOEXEC;call.result(-1);return;}
  uint32_t phdr_size=uint32_t(_ecv_e_phnum*sizeof(Elf32_Phdr));
  uint32_t phdrs=allocate(phdr_size),info=allocate(32),name=copy_string(arena,"",19);
  if((phdr_size&&!phdrs)||!info||!name){if(phdrs)release(phdrs);if(info)release(info);errno=ENOMEM;call.result(-1);return;}
  if(phdr_size)memcpy(guest(arena,phdrs,phdr_size),_ecv_e_ph,phdr_size);
  uint8_t d[32]={};memcpy(d+4,&name,4);memcpy(d+8,&phdrs,4);
  uint16_t count=uint16_t(_ecv_e_phnum);memcpy(d+12,&count,2);
  memcpy(guest(arena,info,sizeof(d)),d,sizeof(d));
  uint32_t args[]={info,32,opaque};invoke(arena,state,runtime,callback,args,3);
  int result=int32_t(state->gpr.rax.dword);release(info);if(phdrs)release(phdrs);call.result(result);
}
IMPORT(dladdr) {
  CALL;uint32_t addr=call.word(0),info=call.word(1);
  if(!info){errno=EFAULT;call.result(0);return;}
  uint32_t base=UINT32_MAX,sym=0,sym_addr=0;bool mapped=false;
  for(uint64_t i=0;i<_ecv_e_phnum;++i){
    Elf32_Phdr p;memcpy(&p,_ecv_e_ph+i*_ecv_e_phent,sizeof(p));
    if(p.p_type!=PT_LOAD)continue;
    if(p.p_vaddr<base)base=p.p_vaddr;
    if(uint64_t(addr)>=p.p_vaddr&&uint64_t(addr)<uint64_t(p.p_vaddr)+p.p_memsz)mapped=true;
  }
  if(!mapped){call.result(0);return;}
  if(base!=UINT32_MAX)base&=~0xfffu;else base=0;
  for(const auto &entry:runtime->addr_fun_symbol_map) {
    if(entry.first>addr||entry.first<sym_addr)continue;
    sym_addr=uint32_t(entry.first);sym=copy_string(arena,entry.second,16);
  }
  uint32_t d[4]={copy_string(arena,"",17),base,sym,sym?sym_addr:0};
  memcpy(guest(arena,info,sizeof(d)),d,sizeof(d));call.result(1);
}
