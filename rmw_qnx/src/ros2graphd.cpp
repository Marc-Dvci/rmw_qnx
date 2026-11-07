// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
//
// ros2graphd: the ROS 2 graph of rmw_qnx as a QNX resource manager.
//
//   ros2graphd [-d DOMAIN]        (default: ROS_DOMAIN_ID, or 0); stays in the foreground
//
//   /dev/ros2/topics/<name>       one file per topic; cat prints its type, endpoint counts and latest message
//   /dev/ros2/services/<name>     one file per service; cat prints its type and server
//
// Every open renders a fresh snapshot from the topics' shared-memory objects (/dev/shmem/rmwqnx.d<domain>.*) and
// the service registry, so ls and cat always show the present. A topic's latest message is decoded with the type's
// own C++ introspection library, loaded by name with dlopen. Names keep ROS's form without the leading '/'; a '/'
// inside a name is shown as %2F (core/names.hpp).
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "cdr.hpp"
#include "names.hpp"
#include "ring.hpp"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "serdes.hpp"
#include "topics.hpp"

namespace its = rosidl_typesupport_introspection_cpp;

namespace {

size_t g_domain = 0;

struct Ocb {
  iofunc_ocb_t hdr;  // first: the iofunc layer frees the OCB with free()
  iofunc_attr_t *own_attr;
  char *data;
  size_t len;
  int is_dir;
  char **entries;
  size_t n_entries;
};

iofunc_attr_t g_root_attr;
resmgr_connect_funcs_t g_connect;
resmgr_io_funcs_t g_io;

// ---- the graph, read from shared memory ----

std::vector<std::string> topic_names() {
  std::vector<std::string> out;
  const std::string prefix = "rmwqnx.d" + std::to_string(g_domain) + ".";
  DIR *d = opendir("/dev/shmem");
  if (!d) return out;
  while (dirent *e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.compare(0, prefix.size(), prefix) != 0) continue;
    auto decoded = rmw_qnx::core::decode_name(n.substr(prefix.size()));
    if (decoded) out.push_back(*decoded);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

std::string services_dir() { return "/tmp/rmwqnx/d" + std::to_string(g_domain) + "/services"; }

std::vector<std::string> service_names() {
  std::vector<std::string> out;
  DIR *d = opendir(services_dir().c_str());
  if (!d) return out;
  while (dirent *e = readdir(d)) {
    const std::string n = e->d_name;
    if (n == "." || n == ".." || n.find(".tmp") != std::string::npos) continue;
    auto decoded = rmw_qnx::core::decode_name(n);
    if (!decoded) continue;
    std::ifstream f(services_dir() + "/" + n);
    long pid = 0;
    if (f >> pid && kill(pid_t(pid), 0) == 0) out.push_back(*decoded);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

// What ls shows: the ROS name without its leading '/', with any other '/' as %2F.
std::string entry_of(const std::string &ros_name) {
  return rmw_qnx::core::encode_name(ros_name.size() && ros_name[0] == '/' ? ros_name.substr(1) : ros_name);
}

// ---- printing a message from its introspection description ----

void printf_to(std::string &out, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  out += buf;
}

void print_prim(std::string &out, uint8_t t, const void *p) {
  switch (t) {
    case its::ROS_TYPE_FLOAT: printf_to(out, "%g", double(*static_cast<const float *>(p))); break;
    case its::ROS_TYPE_DOUBLE: printf_to(out, "%g", *static_cast<const double *>(p)); break;
    case its::ROS_TYPE_BOOLEAN: out += *static_cast<const bool *>(p) ? "true" : "false"; break;
    case its::ROS_TYPE_CHAR:
    case its::ROS_TYPE_INT8: printf_to(out, "%d", int(*static_cast<const int8_t *>(p))); break;
    case its::ROS_TYPE_OCTET:
    case its::ROS_TYPE_UINT8: printf_to(out, "%u", unsigned(*static_cast<const uint8_t *>(p))); break;
    case its::ROS_TYPE_INT16: printf_to(out, "%d", int(*static_cast<const int16_t *>(p))); break;
    case its::ROS_TYPE_WCHAR:
    case its::ROS_TYPE_UINT16: printf_to(out, "%u", unsigned(*static_cast<const uint16_t *>(p))); break;
    case its::ROS_TYPE_INT32: printf_to(out, "%d", *static_cast<const int32_t *>(p)); break;
    case its::ROS_TYPE_UINT32: printf_to(out, "%u", *static_cast<const uint32_t *>(p)); break;
    case its::ROS_TYPE_INT64: printf_to(out, "%lld", static_cast<long long>(*static_cast<const int64_t *>(p))); break;
    case its::ROS_TYPE_UINT64:
      printf_to(out, "%llu", static_cast<unsigned long long>(*static_cast<const uint64_t *>(p)));
      break;
    default: out += "?"; break;
  }
}

void print_message(std::string &out, const its::MessageMembers *mm, const void *msg, int indent) {
  const auto *base = static_cast<const uint8_t *>(msg);
  for (uint32_t i = 0; i < mm->member_count_; i++) {
    const its::MessageMember &m = mm->members_[i];
    const void *field = base + m.offset_;
    out.append(size_t(indent), ' ');
    out += m.name_;
    out += ":";
    const auto *sub = m.type_id_ == its::ROS_TYPE_MESSAGE ? static_cast<const its::MessageMembers *>(m.members_->data)
                                                         : nullptr;
    if (!m.is_array_) {
      if (sub) {
        out += "\n";
        print_message(out, sub, field, indent + 2);
      } else if (m.type_id_ == its::ROS_TYPE_STRING) {
        out += " " + *static_cast<const std::string *>(field) + "\n";
      } else {
        out += " ";
        print_prim(out, m.type_id_, field);
        out += "\n";
      }
      continue;
    }
    const size_t n = m.size_function(field);
    if (sub) {
      printf_to(out, " (%zu items)\n", n);
      for (size_t k = 0; k < n && k < 8; k++) {
        out.append(size_t(indent + 2), ' ');
        out += "-\n";
        print_message(out, sub, m.get_const_function(field, k), indent + 4);
      }
      continue;
    }
    out += " [";
    for (size_t k = 0; k < n && k < 16; k++) {
      if (k) out += ", ";
      if (m.type_id_ == its::ROS_TYPE_STRING) {
        out += *static_cast<const std::string *>(m.get_const_function(field, k));
      } else {
        uint64_t tmp[2] = {0, 0};
        m.fetch_function(field, k, tmp);
        print_prim(out, m.type_id_, tmp);
      }
    }
    if (n > 16) printf_to(out, ", ... %zu in all", n);
    out += "]\n";
  }
}

// The type's C++ introspection description, from lib<pkg>__rosidl_typesupport_introspection_cpp.so.
const its::MessageMembers *members_for(const std::string &type) {
  const size_t a = type.find('/'), b = type.rfind('/');
  if (a == std::string::npos || a == b) return nullptr;
  const std::string pkg = type.substr(0, a), sub = type.substr(a + 1, b - a - 1), name = type.substr(b + 1);
  void *lib = dlopen(("lib" + pkg + "__rosidl_typesupport_introspection_cpp.so").c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!lib) return nullptr;
  using Fn = const rosidl_message_type_support_t *(*)();
  const std::string sym =
      "rosidl_typesupport_introspection_cpp__get_message_type_support_handle__" + pkg + "__" + sub + "__" + name;
  auto fn = reinterpret_cast<Fn>(dlsym(lib, sym.c_str()));
  return fn ? static_cast<const its::MessageMembers *>(fn()->data) : nullptr;
}

std::string render_topic(const std::string &ros_name) {
  std::string out;
  const std::string shm = rmw_qnx::shm_name(g_domain, ros_name);
  const int fd = shm_open(shm.c_str(), O_RDONLY, 0);
  if (fd < 0) return "topic " + ros_name + " is gone\n";
  struct stat st {};
  fstat(fd, &st);
  void *p = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return "cannot map " + shm + "\n";
  auto *h = static_cast<rmw_qnx::TopicHeader *>(p);
  rmw_qnx::core::Ring ring(static_cast<char *>(p) + rmw_qnx::kRingOffset);
  size_t subs = 0;
  for (const auto &s : h->subs) subs += s.state.load() == 2 && kill(s.pid, 0) == 0;
  const uint64_t head = ring.head();
  printf_to(out, "topic:        %s\n", ros_name.c_str());
  printf_to(out, "type:         %s\n", h->type);
  printf_to(out, "publishers:   %u\n", h->pubs.load());
  printf_to(out, "subscribers:  %zu\n", subs);
  printf_to(out, "published:    %llu messages\n", static_cast<unsigned long long>(head));
  printf_to(out, "transport:    shared-memory ring, %u slots of %u bytes, %s\n", ring.count(), ring.payload(),
            shm.c_str());
  if (head == 0) {
    out += "latest:       (none yet)\n";
  } else {
    std::vector<uint8_t> buf(ring.payload());
    rmw_qnx::core::TakeResult t{rmw_qnx::core::TakeStatus::kRetry};
    for (int i = 0; i < 100 && t.status == rmw_qnx::core::TakeStatus::kRetry; i++) {
      uint64_t pos = ring.head() - 1;
      t = ring.take(&pos, buf.data(), uint32_t(buf.size()));
    }
    const its::MessageMembers *mm = members_for(h->type);
    if (t.status != rmw_qnx::core::TakeStatus::kOk || t.length < rmw_qnx::kPrefix) {
      out += "latest:       (being written, try again)\n";
    } else if (!mm) {
      printf_to(out, "latest:       %u bytes of CDR (no C++ introspection library for this type)\n",
                t.length - unsigned(rmw_qnx::kPrefix));
    } else {
      uint64_t pub_seq = 0;
      std::memcpy(&pub_seq, buf.data() + 16, 8);
      printf_to(out, "latest:       message %llu, sequence %llu from its publisher\n",
                static_cast<unsigned long long>(t.seq + 1), static_cast<unsigned long long>(pub_seq));
      std::vector<uint64_t> storage(mm->size_of_ / 8 + 1);
      mm->init_function(storage.data(), rosidl_runtime_cpp::MessageInitialization::ALL);
      rmw_qnx::core::CdrReader r(buf.data() + rmw_qnx::kPrefix, t.length - rmw_qnx::kPrefix);
      if (r.ok() && rmw_qnx::deserialize_cpp(mm, storage.data(), r))
        print_message(out, mm, storage.data(), 2);
      else
        out += "  (could not be decoded)\n";
      mm->fini_function(storage.data());
    }
  }
  munmap(p, size_t(st.st_size));
  return out;
}

std::string render_service(const std::string &ros_name) {
  std::ifstream f(services_dir() + "/" + rmw_qnx::core::encode_name(ros_name));
  long pid = 0, chid = 0;
  unsigned long index = 0;
  if (!(f >> pid >> chid >> index)) return "service " + ros_name + " is gone\n";
  std::string out;
  printf_to(out, "service:      %s\n", ros_name.c_str());
  printf_to(out, "server:       pid %ld, channel %ld, service %lu\n", pid, chid, index);
  out += "transport:    MsgSend/MsgReply: the server's thread runs at the calling client's priority\n";
  return out;
}

// ---- the resource manager ----

const char *const kRootEntries[] = {"topics", "services"};

void set_entries(Ocb *o, const std::vector<std::string> &names) {
  o->n_entries = names.size();
  o->entries = static_cast<char **>(calloc(names.size() + 1, sizeof(char *)));
  for (size_t i = 0; i < names.size(); i++) o->entries[i] = strdup(names[i].c_str());
}

int io_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra) {
  (void)extra;
  const std::string path = msg->connect.path;  // relative to /dev/ros2
  std::vector<std::string> entries;
  std::string data;
  bool dir = false;
  if (path.empty()) {
    dir = true;
    entries.assign(std::begin(kRootEntries), std::end(kRootEntries));
  } else if (path == "topics" || path == "services") {
    dir = true;
    for (const std::string &n : path == "topics" ? topic_names() : service_names()) entries.push_back(entry_of(n));
  } else {
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return ENOENT;
    const std::string kind = path.substr(0, slash), entry = path.substr(slash + 1);
    auto decoded = rmw_qnx::core::decode_name(entry);
    if (!decoded || (kind != "topics" && kind != "services")) return ENOENT;
    const std::string ros_name = "/" + *decoded;
    const std::vector<std::string> names = kind == "topics" ? topic_names() : service_names();
    if (std::find(names.begin(), names.end(), ros_name) == names.end()) return ENOENT;
    data = kind == "topics" ? render_topic(ros_name) : render_service(ros_name);
  }
  if (!dir && (msg->connect.ioflag & O_ACCMODE) != O_RDONLY) return EROFS;
  auto *o = static_cast<Ocb *>(calloc(1, sizeof(Ocb)));
  if (!o) return ENOMEM;
  iofunc_attr_t *attr = static_cast<iofunc_attr_t *>(handle);
  if (!dir) {
    o->own_attr = static_cast<iofunc_attr_t *>(calloc(1, sizeof(iofunc_attr_t)));
    iofunc_attr_init(o->own_attr, S_IFREG | 0444, nullptr, nullptr);
    o->own_attr->nbytes = off_t(data.size());
    attr = o->own_attr;
    o->data = static_cast<char *>(malloc(data.size() + 1));
    std::memcpy(o->data, data.data(), data.size());
    o->len = data.size();
  } else {
    set_entries(o, entries);
    o->is_dir = 1;
  }
  const int r = iofunc_ocb_attach(ctp, msg, &o->hdr, attr, nullptr);
  if (r != EOK) {
    free(o->data);
    free(o->own_attr);
    free(o);
  }
  return r;
}

int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb_) {
  Ocb *o = reinterpret_cast<Ocb *>(ocb_);
  int st = iofunc_read_verify(ctp, msg, &o->hdr, nullptr);
  if (st != EOK) return st;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  const size_t want = msg->i.nbytes;
  if (!o->is_dir) {
    const size_t off = size_t(o->hdr.offset);
    const size_t n = off >= o->len ? 0 : std::min(want, o->len - off);
    o->hdr.offset += off_t(n);
    _IO_SET_READ_NBYTES(ctp, n);
    if (n) SETIOV(ctp->iov, o->data + off, n);
    return _RESMGR_NPARTS(n ? 1 : 0);
  }
  // Directory: one struct dirent per entry, the OCB's offset counting entries.
  std::vector<char> buf(want);
  size_t used = 0;
  while (size_t(o->hdr.offset) < o->n_entries) {
    const char *name = o->entries[o->hdr.offset];
    const size_t reclen = (offsetof(struct dirent, d_name) + strlen(name) + 1 + 7) & ~size_t{7};
    if (used + reclen > want) break;
    auto *d = reinterpret_cast<struct dirent *>(buf.data() + used);
    std::memset(d, 0, reclen);
    d->d_ino = ino_t(o->hdr.offset + 2);
    d->d_offset = o->hdr.offset;
    d->d_reclen = int16_t(reclen);
    d->d_namelen = int16_t(strlen(name));
    std::strcpy(d->d_name, name);
    used += reclen;
    o->hdr.offset++;
  }
  _IO_SET_READ_NBYTES(ctp, used);
  MsgReply(ctp->rcvid, ssize_t(used), buf.data(), used);
  return _RESMGR_NOREPLY;
}

int io_close_ocb(resmgr_context_t *ctp, void *reserved, RESMGR_OCB_T *ocb_) {
  Ocb *o = reinterpret_cast<Ocb *>(ocb_);
  iofunc_attr_t *own = o->own_attr;
  free(o->data);
  for (size_t i = 0; i < o->n_entries; i++) free(o->entries[i]);
  free(o->entries);
  const int r = iofunc_close_ocb_default(ctp, reserved, &o->hdr);
  free(own);
  return r;
}

}  // namespace

int main(int argc, char **argv) {
  const char *env = std::getenv("ROS_DOMAIN_ID");
  if (env) g_domain = size_t(std::atoi(env));
  for (int i = 1; i + 1 < argc; i++)
    if (!std::strcmp(argv[i], "-d")) g_domain = size_t(std::atoi(argv[i + 1]));
  dispatch_t *dpp = dispatch_create();
  if (!dpp) {
    std::perror("dispatch_create");
    return 1;
  }
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect, _RESMGR_IO_NFUNCS, &g_io);
  g_connect.open = io_open;
  g_io.read = io_read;
  g_io.close_ocb = io_close_ocb;
  iofunc_attr_init(&g_root_attr, S_IFDIR | 0555, nullptr, nullptr);
  resmgr_attr_t rattr{};
  rattr.nparts_max = 1;
  rattr.msg_max_size = 4096;
  if (resmgr_attach(dpp, &rattr, "/dev/ros2", _FTYPE_ANY, _RESMGR_FLAG_DIR, &g_connect, &g_io, &g_root_attr) == -1) {
    std::perror("resmgr_attach /dev/ros2");
    return 1;
  }
  std::printf("ros2graphd: /dev/ros2 for ROS domain %zu\n", g_domain);
  std::fflush(stdout);
  dispatch_context_t *ctp = dispatch_context_alloc(dpp);
  for (;;) {
    if (!(ctp = dispatch_block(ctp))) continue;
    dispatch_handler(ctp);
  }
}
